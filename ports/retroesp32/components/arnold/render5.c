/*
 * render5.c  –  ESP32/GX4000 line-rendering version
 *
 * Original: Arnold emulator (c) Kevin Thacker 1995-2001, GPL-2.0
 * Ported/adapted for ESP32-S3 (no PSRAM) by CardputerEmu.
 *
 * Key changes vs. the original GCW0 render5.c:
 *
 *  1. LINE BUFFER ONLY:
 *       InitialiseRender() allocates ONE scan line (ScreenPitch bytes) via
 *       heap_caps_malloc instead of 313 × ScreenPitch (≈ 626 KB at 16bpp).
 *
 *  2. PER-LINE PUSH:
 *       Render_NextLine() calls gx4000_display_flush_line() to push the
 *       just-completed line to the LCD before resetting pScreenLine to the
 *       start of the single-line buffer.
 *
 *  3. SIMPLIFIED FRAME-END:
 *       Render_DumpScreen4() (called once/frame via Render_DumpDisplay) only
 *       calls Host_SwapGraphicsBuffers(); lines are already on the LCD.
 *
 *  4. HEAP-ALLOCATED TABLE POINTERS:
 *       render.c (this port) provides pointer-based table accessors; this
 *       file calls them the same way as the original.
 *
 *  5. STATIC COLOUR TABLES remain small (32-entry × 4 B = 128 B each).
 */

#pragma GCC optimize ("Ofast")

#include "cpcglob.h"
#include "cpc.h"
#include "render.h"
#include "host.h"

/* gx4000_display_flush_line() lives in gx4000_display.cpp */
#include "../gx4000_display.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

/* ── Static (small) runtime state ──────────────────────────────────────── */

static BOOL          Renderer_Active  = FALSE;
static int           BytesPerPixel    = 2;
int                  ScanLines        = 0;
int                  FillScanLines    = 0;
static int           PIXEL_STEP       = 2;
int                  PIXEL_STEP_SHIFT = 1;
static int           Render_CPCRenderHeight, Render_CPCRenderWidth;
static int           Render_CPCXOffset, Render_CPCYOffset;

/* Internal single-line render buffer (heap) */
static unsigned char *pScreenBase  = NULL;
static unsigned long  ScreenPitch  = 0;
static unsigned long  ScreenHeight = 0;   /* always 1 in our implementation */
static unsigned char *pScreenLine  = NULL;

/* Actual CPC scan-line counter (0 .. LINES_PER_SCREEN) */
static int ActualScanLine = 0;

/* Current pixel format for render loop */
static int CurrentGraphicsFormatBPP = 16;

/* ── Colour tables (32-entry – stays in SRAM, only 128 B each) ─────────── */

/* ConvertedColourTable[pen] = packed RGB565 value */
static uint16_t ConvertedColourTable[32];

typedef struct {
    unsigned char Red, Green, Blue, pad;
} RGB888;
static RGB888 UnConvertedColourTable[32];

/* Palette remapping (paletted mode – not used in 16bpp TrueColour) */
static signed long PaletteRemap[32];

static PALETTE_ENTRY CPCPalette[256];

/* A CPC line is useful only if it maps to a new LCD row in 272->135 scaling. */
static BOOL gx4000_render_is_useful_scanline(int actualScanLine)
{
    if (actualScanLine < Render_CPCYOffset ||
        actualScanLine >= Render_CPCYOffset + Render_CPCRenderHeight) {
        return FALSE;
    }

    {
        int visY = actualScanLine - Render_CPCYOffset;
        if (visY <= 0) {
            return TRUE;
        }

        {
            int lcdY     = (int)(((unsigned)visY * GX4000_LCD_H) / GX4000_CPC_VISIBLE_H);
            int prevLcdY = (int)((((unsigned)visY - 1U) * GX4000_LCD_H) / GX4000_CPC_VISIBLE_H);
            return (lcdY != prevLcdY) ? TRUE : FALSE;
        }
    }
}

static void build_colour_tables(void)
{
    /* No-op: conversion tables removed to save static RAM. */
}

static uint16_t pack_rgb565(unsigned char r, unsigned char g, unsigned char b)
{
    return (uint16_t)(((uint16_t)(r >> 3) << 11) |
                      ((uint16_t)(g >> 2) << 5) |
                      ((uint16_t)(b >> 3)));
}

/* ── Current mode pixel data (pointers from render.c) ──────────────────── */

static PIXEL_DATA   *pPixelData     = NULL;
static unsigned long *pPackedPixels = NULL;
static int  CurrentMode             = 1;
static int  CurrentModeMask         = 0x0eeeeeeee;
static int  CurrentModeShift        = 1;
static int  CurrentTopShift         = 5;
static int  ModeHorizontalPixelScroll = 0;
static int  HorizontalPixelScroll   = 0;

/* Forward declaration used by Render_SetDisplay* before body definition. */
void Render_DumpScreen4(void);

/* ── Render function pointers ──────────────────────────────────────────── */

static void Render_SetColourNULL(const RGBCOLOUR *c, int i)   { (void)c; (void)i; }
static void Render_PutDataWordNULL(int h, unsigned long d, int l) { (void)h; (void)d; (void)l; }
static void Render_PutSyncNULL(int h, int l)   { (void)h; (void)l; }
static void Render_PutBorderNULL(int h, int l) { (void)h; (void)l; }
static void Render_PutDataWordPLUSNULL(int h, unsigned long d, int l,
                                        unsigned long m, int *p)
{ (void)h; (void)d; (void)l; (void)m; (void)p; }

static void (*pRender_DumpScreen)(void) = NULL;
static void (*pRender_SetColour)(const RGBCOLOUR *, int) = Render_SetColourNULL;
static void (*pRender_PutDataWord)(int, unsigned long, int) = Render_PutDataWordNULL;
static void (*pRender_PutSync)(int, int) = Render_PutSyncNULL;
static void (*pRender_PutBorder)(int, int) = Render_PutBorderNULL;
static void (*pRender_PutDataWordPLUS)(int, unsigned long, int, unsigned long, int *)
    = Render_PutDataWordPLUSNULL;

/* ── RenderScreenLine address macros (LESS_MULTS mode) ─────────────────── */
/*
 * Render_CalcRenderAddress: pScreen points to current NOP position in line.
 * Render_UpdateRenderAddress: store back after writing pixels.
 */
#define Render_CalcRenderAddress(HCount, Line) \
    unsigned char *pScreen = (unsigned char *)pScreenLine

#define Render_UpdateRenderAddress \
    pScreenLine = pScreen

/* ── TrueColour (16-bpp) render functions ─────────────────────────────── */

#define TC_WRITE(pScr, ci)  \
    { unsigned short _v = ConvertedColourTable[ci]; \
      *((unsigned short *)(pScr)) = _v; }

void Render_TrueColourRGB_SetColour(const RGBCOLOUR *pColour, int Index)
{
    UnConvertedColourTable[Index & 31].Red   = pColour->u.element.Red;
    UnConvertedColourTable[Index & 31].Green = pColour->u.element.Green;
    UnConvertedColourTable[Index & 31].Blue  = pColour->u.element.Blue;
    ConvertedColourTable[Index & 31] = pack_rgb565(pColour->u.element.Red,
                                                    pColour->u.element.Green,
                                                    pColour->u.element.Blue);
}

void Render_TrueColourRGB_PutDataWord(int HorizontalCount,
                                       unsigned long GraphicsData, int Line)
{
    int i;
    PIXEL_DATA *pThisPixelData;
    Render_CalcRenderAddress(HorizontalCount, Line);

    pThisPixelData = &pPixelData[(GraphicsData >> 8) & 0xFF];
    for (i = 0; i < 8; i += PIXEL_STEP) {
        TC_WRITE(pScreen, pThisPixelData->Pixel[i]);
        pScreen += BytesPerPixel;
    }
    pThisPixelData = &pPixelData[GraphicsData & 0xFF];
    for (i = 0; i < 8; i += PIXEL_STEP) {
        TC_WRITE(pScreen, pThisPixelData->Pixel[i]);
        pScreen += BytesPerPixel;
    }
    Render_UpdateRenderAddress;
}

void Render_TrueColourRGB_PutSync(int HorizontalCount, int Line)
{
    int i;
    Render_CalcRenderAddress(HorizontalCount, Line);
    for (i = 0; i < 16; i += PIXEL_STEP) {
        *((unsigned short *)pScreen) = 0;
        pScreen += BytesPerPixel;
    }
    Render_UpdateRenderAddress;
}

void Render_TrueColourRGB_PutBorder(int HorizontalCount, int Line)
{
    int i;
    unsigned short bv = (unsigned short)ConvertedColourTable[16];
    Render_CalcRenderAddress(HorizontalCount, Line);
    for (i = 0; i < 16; i += PIXEL_STEP) {
        *((unsigned short *)pScreen) = bv;
        pScreen += BytesPerPixel;
    }
    Render_UpdateRenderAddress;
}

void Render_TrueColourRGB_PutBorderCycles(int HorizontalCount, int Line,
                                           int Cycles)
{
    int i;
    unsigned short bv = (unsigned short)ConvertedColourTable[16];
    Render_CalcRenderAddress(HorizontalCount, Line);
    for (i = 0; i < Cycles; i += PIXEL_STEP) {
        *((unsigned short *)pScreen) = bv;
        pScreen += BytesPerPixel;
    }
    Render_UpdateRenderAddress;
}

void Render_TrueColourRGB_PutDataWordPLUS(int HorizontalCount,
                                           unsigned long GraphicsData,
                                           int Line,
                                           unsigned long Mask,
                                           int *pPixels)
{
    int i;
    PIXEL_DATA *pThisPixelData;
    Render_CalcRenderAddress(HorizontalCount, Line);

    GraphicsData >>= (ModeHorizontalPixelScroll >> 2);

    pThisPixelData = &pPixelData[(GraphicsData >> 8) & 0xFF];
    for (i = 0; i < 8; i += PIXEL_STEP) {
        int Pixel = ((Mask & (1 << i)) != 0)
                    ? (int)pThisPixelData->Pixel[i] : pPixels[i];
        TC_WRITE(pScreen, Pixel);
        pScreen += BytesPerPixel;
    }
    pThisPixelData = &pPixelData[GraphicsData & 0xFF];
    for (i = 0; i < 8; i += PIXEL_STEP) {
        int Pixel = ((Mask & (1 << (i + 8))) != 0)
                    ? (int)pThisPixelData->Pixel[i] : pPixels[i + 8];
        TC_WRITE(pScreen, Pixel);
        pScreen += BytesPerPixel;
    }
    Render_UpdateRenderAddress;
}

/* ── Paletted render stubs (not used in TrueColour mode) ─────────────── */

void Render_Paletted_PutDataWord(int h, unsigned long d, int l)
{ (void)h; (void)d; (void)l; }
void Render_Paletted_PutSync(int h, int l)   { (void)h; (void)l; }
void Render_Paletted_PutBorder(int h, int l) { (void)h; (void)l; }
void Render_Paletted_PutDataWordPLUS(int h, unsigned long d, int l,
                                      unsigned long m, int *p)
{ (void)h; (void)d; (void)l; (void)m; (void)p; }

/* ── TrueColour mode setup ──────────────────────────────────────────────── */

static void Render_TrueColourRGB_Setup(void)
{
    build_colour_tables();
    pRender_SetColour       = Render_TrueColourRGB_SetColour;
    pRender_PutSync         = Render_TrueColourRGB_PutSync;
    pRender_PutBorder       = Render_TrueColourRGB_PutBorder;
    pRender_PutDataWord     = Render_TrueColourRGB_PutDataWord;
    pRender_PutDataWordPLUS = Render_TrueColourRGB_PutDataWordPLUS;
    CRTC_SetTrueColourRender(TRUE);
}

/* ── Palette helpers (small arrays, TrueColour path) ──────────────────── */

void Palette_Initialise(void)
{
    int i;
    for (i = 0; i < 32; i++) PaletteRemap[i] = -1;
}

void Palette_Set(void) { /* no-op in TrueColour mode */ }
void Palette_Reset(void) {
    int i;
    for (i = 0; i < 256; i++)
        if (!(CPCPalette[i].Flags & PALETTE_ENTRY_DO_NOT_REMOVE))
            CPCPalette[i].Flags = 0;
    for (i = 0; i < 32; i++) PaletteRemap[i] = -1;
    CPC_UpdateColours();
}

void Render_MarkPaletteEntryForHostUse(int Index)
{
    if (Index < 0 || Index > 255) return;
    CPCPalette[Index].Red   = (unsigned char)Index;
    CPCPalette[Index].Green = 0;
    CPCPalette[Index].Blue  = 0;
    CPCPalette[Index].Flags = PALETTE_ENTRY_USED
                            | PALETTE_ENTRY_DO_NOT_MATCH
                            | PALETTE_ENTRY_DO_NOT_REMOVE;
}

/* ── Pixel translation (mode switch) ──────────────────────────────────── */

void Render_SetPixelTranslation(int ModeIndex)
{
    CurrentMode = ModeIndex;
    switch (ModeIndex) {
        case 0: CurrentModeMask = 0x0aaaaaaaa; CurrentModeShift = 2; CurrentTopShift = 7; break;
        case 1: CurrentModeMask = 0x0eeeeeeee; CurrentModeShift = 1; CurrentTopShift = 5; break;
        case 2: CurrentModeMask = 0x07f7f7f7f; CurrentModeShift = 0; CurrentTopShift = 1; break;
        default: CurrentModeMask = 0x0eeeeeeee; CurrentModeShift = 1; CurrentTopShift = 5; break;
    }
    pPixelData    = CPC_GetModePixelData(ModeIndex);
    pPackedPixels = CPC_GetModePackedPixelData(ModeIndex);
}

void Render_SetHorizontalPixelScroll(int PixelScroll)
{
    HorizontalPixelScroll = PixelScroll;
    ModeHorizontalPixelScroll = HorizontalPixelScroll >> CurrentModeShift;
}

/* ── Public render API forwarding ──────────────────────────────────────── */

void Render_SetColour(const RGBCOLOUR *pColour, int Index)
{
    UnConvertedColourTable[Index & 31].Red   = pColour->u.element.Red;
    UnConvertedColourTable[Index & 31].Green = pColour->u.element.Green;
    UnConvertedColourTable[Index & 31].Blue  = pColour->u.element.Blue;
    pRender_SetColour(pColour, Index);
}

void Render_PutDataWord(int H, unsigned long D, int L)
{ pRender_PutDataWord(H, D, L); }

void Render_PutSync(int H, int L)
{ pRender_PutSync(H, L); }

void Render_PutBorder(int H, int L)
{ pRender_PutBorder(H, L); }

void Render_PutDataWordPLUSMaskWithPixels(int H, unsigned long D, int L,
                                           unsigned long M, int *P)
{ pRender_PutDataWordPLUS(H, D, L, M, P); }

/* ── Rendering accuracy ─────────────────────────────────────────────────── */

int Render_RenderingAccuracyForWindowedMode = RENDERING_ACCURACY_LOW;

void Render_SetRenderingAccuracy(int Accuracy)
{
    if (Accuracy == RENDERING_ACCURACY_LOW) {
        PIXEL_STEP = 2; PIXEL_STEP_SHIFT = 1;
        ScanLines = 0; FillScanLines = 0;
    } else {
        /* High / Higher: use low on constrained hardware */
        PIXEL_STEP = 2; PIXEL_STEP_SHIFT = 1;
        ScanLines = 0; FillScanLines = 0;
    }
}

void Render_SetRenderingAccuracyForWindowedMode(int Accuracy)
{
    Render_RenderingAccuracyForWindowedMode = Accuracy;
    Render_SetRenderingAccuracy(Accuracy);
}

/* ── NULL stubs ─────────────────────────────────────────────────────────── */

void Render_ClearDisplay(void)
{
    if (pScreenBase) memset(pScreenBase, 0, ScreenPitch);
}

BOOL Render_IsRenderActive(void)   { return Renderer_Active; }
BOOL Render_IsRendererActive(void) { return Renderer_Active; }

/* ── Initialise single-line render buffer ──────────────────────────────── */
/*
 * Called from Render_SetDisplayFullScreen / Render_SetDisplayWindowed.
 * ScreenResX / ScreenResY are the requested display resolution (e.g. 384×272).
 * We always allocate ONE scan line.
 */
static BOOL InitialiseRender(int ScreenResX, int ScreenResY, int BPP)
{
    int CPCXOffset, CPCYOffset, CPCScreenWidth, CPCScreenHeight;

    /* Release any previous buffer */
    if (pScreenBase) {
        heap_caps_free(pScreenBase);
        pScreenBase = NULL;
    }

    BytesPerPixel = (BPP + 7) >> 3;
    if (BytesPerPixel < 1) BytesPerPixel = 2;

    /* Internal pitch: full scan line width × bytes/pixel
     * BITS_PER_LINE = 1024 at PIXEL_STEP=1; we use PIXEL_STEP=2 so half
     * the pixels are written per NOP, but pitch stays at BITS_PER_LINE
     * (Arnold render offsets are pre-halved by PIXEL_STEP_SHIFT).      */
    ScreenPitch  = (unsigned long)BITS_PER_LINE * (unsigned long)BytesPerPixel;
    ScreenHeight = 1;  /* ← KEY CHANGE: one line only */

    /* Allocate the single-line buffer in internal SRAM (DMA-capable) */
    pScreenBase = (unsigned char *)heap_caps_malloc(
        ScreenPitch + 4,   /* +4 guard for 4-byte write overshoot */
        MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);

    if (!pScreenBase) {
        Renderer_Active = FALSE;
        return FALSE;
    }
    memset(pScreenBase, 0, ScreenPitch + 4);

    /* ── Compute visible-area clipping parameters ───────────────────────── */

    CPCXOffset      = X_CRTC_CHAR_OFFSET << (1 + 3);            /* full-res pixels */
    CPCYOffset      = Y_CRTC_LINE_OFFSET;                        /* scan lines      */
    CPCScreenWidth  = X_CRTC_CHAR_WIDTH  << (1 + 3);            /* full-res pixels */
    CPCScreenHeight = Y_CRTC_LINE_HEIGHT;                        /* scan lines      */

    CPCScreenWidth >>= PIXEL_STEP_SHIFT;
    CPCXOffset     >>= PIXEL_STEP_SHIFT;

    /* Horizontal */
    if (ScreenResX >= CPCScreenWidth) {
        Render_CPCRenderWidth  = CPCScreenWidth;
        Render_CPCXOffset      = CPCXOffset;
    } else {
        Render_CPCRenderWidth  = ScreenResX;
        Render_CPCXOffset      = CPCXOffset + (CPCScreenWidth >> 1) - (ScreenResX >> 1);
    }

    /* Vertical */
    if (ScreenResY >= CPCScreenHeight) {
        Render_CPCRenderHeight = CPCScreenHeight;
        Render_CPCYOffset      = CPCYOffset;
    } else {
        Render_CPCRenderHeight = ScreenResY;
        Render_CPCYOffset      = CPCYOffset + ((CPCScreenHeight - ScreenResY) >> 1);
    }

    ActualScanLine  = 0;
    pScreenLine     = pScreenBase;
    Renderer_Active = TRUE;

    return TRUE;
}

/* ── SetDisplay helpers ─────────────────────────────────────────────────── */

BOOL Render_SetDisplayFullScreen(int Width, int Height, int Depth)
{
    Render_SetRenderingAccuracy(RENDERING_ACCURACY_LOW);

    if (!Host_SetDisplay(DISPLAY_TYPE_FULLSCREEN, Width, Height, Depth))
        return FALSE;

    {
        GRAPHICS_BUFFER_COLOUR_FORMAT *fmt = Host_GetGraphicsBufferColourFormat();
        CurrentGraphicsFormatBPP = fmt->BPP ? fmt->BPP : 16;
    }

    /* Always use TrueColour (16bpp) for our ESP32 target */
    Render_TrueColourRGB_Setup();
    pRender_DumpScreen = Render_DumpScreen4;

    return InitialiseRender(Width, Height, 16);
}

BOOL Render_SetDisplayWindowed(void)
{
    int W = (X_CRTC_CHAR_WIDTH << (1 + 3)) >> PIXEL_STEP_SHIFT;
    int H = Y_CRTC_LINE_HEIGHT;

    Render_SetRenderingAccuracy(Render_RenderingAccuracyForWindowedMode);

    if (!Host_SetDisplay(DISPLAY_TYPE_WINDOWED, W, H, 0))
        return FALSE;

    {
        GRAPHICS_BUFFER_COLOUR_FORMAT *fmt = Host_GetGraphicsBufferColourFormat();
        CurrentGraphicsFormatBPP = fmt->BPP ? fmt->BPP : 16;
    }

    Render_TrueColourRGB_Setup();
    pRender_DumpScreen = Render_DumpScreen4;

    return InitialiseRender(W, H, 16);
}

/* ── Line tracking ──────────────────────────────────────────────────────── */

void Render_FirstLine(void)
{
    ActualScanLine = 0;
    pScreenLine    = pScreenBase;
}

/*
 * Render_NextLine  –  called once per CPC scan line from the CRTC.
 *
 * If the just-rendered line falls in the visible window, push it to the LCD.
 * Then reset pScreenLine to pScreenBase (our single-line buffer overwrite).
 */
void Render_NextLine(void)
{
    /* Flush if within the visible CPC vertical window, and only when
     * Arnold's render state is active (skip frameskip'd frames). */
    if (!CRTC_InternalState.DontRender &&
        pScreenBase &&
        ActualScanLine >= Render_CPCYOffset &&
        ActualScanLine < Render_CPCYOffset + Render_CPCRenderHeight)
    {
        int visY = ActualScanLine - Render_CPCYOffset;
        gx4000_display_flush_line(visY,
                                   pScreenBase,
                                   Render_CPCXOffset,
                                   Render_CPCRenderWidth,
                                   BytesPerPixel);
    }

    /* Increment actual scan line; wrap at full CPC frame height */
    ActualScanLine++;
    if (ActualScanLine >= LINES_PER_SCREEN + 1)
        ActualScanLine = 0;

    /* Always reset to the single-line buffer start */
    pScreenLine = pScreenBase;
}

/* ── Frame end ──────────────────────────────────────────────────────────── */
/*
 * Render_DumpScreen4  –  called once per frame via Render_DumpDisplay().
 * All lines are already on the LCD (pushed in Render_NextLine).
 * We just notify the host that the frame is done.
 */
void Render_DumpScreen4(void)
{
    Host_SwapGraphicsBuffers();   /* → gx4000_display_frame_done() */
}

void Render_DumpDisplay(void)
{
    if (pRender_DumpScreen) pRender_DumpScreen();
}

/* ── Cleanup ────────────────────────────────────────────────────────────── */

void Render_Finish(void)
{
    extern void Render_ESP32_FreeRenderTables(void);
    Render_ESP32_FreeRenderTables();   /* free 40 KB of mode decode tables */

    if (pScreenBase) {
        heap_caps_free(pScreenBase);
        pScreenBase = NULL;
    }
    ScreenPitch     = 0;
    Renderer_Active = FALSE;
}

void Render_Initialise(void)
{
    CPC_BuildModeRenderTables();
    pScreenBase     = NULL;
    ScreenPitch     = 0;
    Renderer_Active = FALSE;
    Palette_Initialise();
    Render_SetRenderingAccuracyForWindowedMode(RENDERING_ACCURACY_LOW);
}

/* ── Export single-line buffer info for run_gx4000.cpp ─────────────────── */

void GX4000_Render_GetLineBuffer(unsigned char **ppBuf,
                                  int *pPitch,
                                  int *pXOffset,
                                  int *pRenderW)
{
    if (ppBuf)   *ppBuf   = pScreenBase;
    if (pPitch)  *pPitch  = (int)ScreenPitch;
    if (pXOffset) *pXOffset = Render_CPCXOffset;
    if (pRenderW) *pRenderW = Render_CPCRenderWidth;
}

/* ── Misc required stubs ────────────────────────────────────────────────── */

void Render_GetPixelRGBAtXY(int X, int Y, unsigned char *r,
                              unsigned char *g, unsigned char *b)
{ (void)X; (void)Y; *r = *g = *b = 0; }

void Render_PlotText(char *pString, int X, int Y)
{ (void)pString; (void)X; (void)Y; }
