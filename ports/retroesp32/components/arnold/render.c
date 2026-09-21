/*
 * render.c  –  ESP32/GX4000 version
 *
 * Original: Arnold emulator (c) Kevin Thacker 1995-2001, GPL-2.0
 * Modified for ESP32-S3 (no PSRAM):
 *   - Mode pixel decode tables are HEAP-ALLOCATED (heap_caps_malloc) instead
 *     of static BSS arrays.  The original file has 4×PIXEL_DATA[256] = 32 KB
 *     of BSS that would inflate boot RAM at link time.
 *   - Tables are allocated in CPC_BuildModeRenderTables() and freed in
 *     Render_ESP32_FreeRenderTables() which is called by Render_Finish().
 */

#pragma GCC optimize ("Ofast")

#include "render.h"
#include "esp_heap_caps.h"   /* heap_caps_malloc / heap_caps_free */
#include <string.h>
#include <stdlib.h>          /* NULL */

/* ── Heap-allocated pixel decode tables ─────────────────────────────────── */
static PIXEL_DATA     *Mode0PixelData    = NULL; /* 256 × 32 B =  8 KB each */
static PIXEL_DATA     *Mode1PixelData    = NULL;
static PIXEL_DATA     *Mode2PixelData    = NULL;
static PIXEL_DATA     *Mode3PixelData    = NULL;

static unsigned long  *Mode0PackedPixels = NULL; /* 256 × 4 B = 1 KB each */
static unsigned long  *Mode1PackedPixels = NULL;
static unsigned long  *Mode2PackedPixels = NULL;
static unsigned long  *Mode3PackedPixels = NULL;

unsigned long         *Mode0PenIndex     = NULL; /* 256 × 4 B = 1 KB each */
unsigned long         *Mode1PenIndex     = NULL;
unsigned long         *Mode2PenIndex     = NULL;
unsigned long         *Mode3PenIndex     = NULL;

#define PACK8_PIXELS(a,b,c,d,e,f,g,h) \
    ((a)|(b<<4)|(c<<8)|(d<<12)|(e<<16)|(f<<20)|(g<<24)|(h<<28))

/* ── Table getters (called from render5.c) ───────────────────────────────── */

unsigned long *CPC_GetModePenIndexTable(int ModeIndex)
{
    switch (ModeIndex) {
        case 1:  return Mode1PenIndex;
        case 2:  return Mode2PenIndex;
        case 3:  return Mode3PenIndex;
        default: return Mode0PenIndex;
    }
}

PIXEL_DATA *CPC_GetModePixelData(int ModeIndex)
{
    switch (ModeIndex) {
        case 1:  return Mode1PixelData;
        case 2:  return Mode2PixelData;
        case 3:  return Mode3PixelData;
        default: return Mode0PixelData;
    }
}

unsigned long *CPC_GetModePackedPixelData(int ModeIndex)
{
    switch (ModeIndex) {
        case 1:  return Mode1PackedPixels;
        case 2:  return Mode2PackedPixels;
        case 3:  return Mode3PackedPixels;
        default: return Mode0PackedPixels;
    }
}

/* ── Allocation helper ───────────────────────────────────────────────────── */

static void *alloc_table(size_t bytes)
{
    void *p = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!p) {
        /* Fallback to regular heap (PSRAM if available, but we target no-PSRAM) */
        p = malloc(bytes);
    }
    return p;
}

/* ── Build (allocate + fill) mode render tables ──────────────────────────── */

void CPC_BuildModeRenderTables(void)
{
    int i;

    /* Allocate all tables */
    Mode0PixelData    = (PIXEL_DATA *)    alloc_table(256 * sizeof(PIXEL_DATA));
    Mode1PixelData    = (PIXEL_DATA *)    alloc_table(256 * sizeof(PIXEL_DATA));
    Mode2PixelData    = (PIXEL_DATA *)    alloc_table(256 * sizeof(PIXEL_DATA));
    Mode3PixelData    = (PIXEL_DATA *)    alloc_table(256 * sizeof(PIXEL_DATA));
    Mode0PackedPixels = (unsigned long *) alloc_table(256 * sizeof(unsigned long));
    Mode1PackedPixels = (unsigned long *) alloc_table(256 * sizeof(unsigned long));
    Mode2PackedPixels = (unsigned long *) alloc_table(256 * sizeof(unsigned long));
    Mode3PackedPixels = (unsigned long *) alloc_table(256 * sizeof(unsigned long));
    Mode0PenIndex     = (unsigned long *) alloc_table(256 * sizeof(unsigned long));
    Mode1PenIndex     = (unsigned long *) alloc_table(256 * sizeof(unsigned long));
    Mode2PenIndex     = (unsigned long *) alloc_table(256 * sizeof(unsigned long));
    Mode3PenIndex     = (unsigned long *) alloc_table(256 * sizeof(unsigned long));

    if (!Mode0PixelData || !Mode1PixelData || !Mode2PixelData || !Mode3PixelData
     || !Mode0PackedPixels || !Mode1PackedPixels
     || !Mode2PackedPixels || !Mode3PackedPixels
     || !Mode0PenIndex || !Mode1PenIndex
     || !Mode2PenIndex || !Mode3PenIndex) {
        /* Critical allocation failure – tables stay NULL,
           the emulator will not render but won't crash. */
        return;
    }

    /* ── Mode 0: 160 pixels, 16 colours ─────────────────────────────────── */
    for (i = 0; i < 256; i++) {
        int P1 = ((i & 0x02) << 2) | ((i & 0x20) >> 3) |
                 ((i & 0x08) >> 2) | ((i & 0x80) >> 7);
        int P2 = ((i & 0x01) << 3) | ((i & 0x10) >> 2) |
                 ((i & 0x04) >> 1) | ((i & 0x40) >> 6);
        Mode0PixelData[i].Pixel[0] = P1;
        Mode0PixelData[i].Pixel[1] = P1;
        Mode0PixelData[i].Pixel[2] = P1;
        Mode0PixelData[i].Pixel[3] = P1;
        Mode0PixelData[i].Pixel[4] = P2;
        Mode0PixelData[i].Pixel[5] = P2;
        Mode0PixelData[i].Pixel[6] = P2;
        Mode0PixelData[i].Pixel[7] = P2;
        Mode0PackedPixels[i] = PACK8_PIXELS(P1,P1,P1,P1,P2,P2,P2,P2);
        Mode0PenIndex[i]     = (unsigned long)P1;
    }

    /* ── Mode 1: 320 pixels, 4 colours ──────────────────────────────────── */
    for (i = 0; i < 256; i++) {
        int P1 = ((i & 0x08) >> 2) | ((i & 0x80) >> 7);
        int P2 = ((i & 0x04) >> 1) | ((i & 0x40) >> 6);
        int P3 = ((i & 0x02)     ) | ((i & 0x20) >> 5);
        int P4 = ((i & 0x01) << 1) | ((i & 0x10) >> 4);
        Mode1PixelData[i].Pixel[0] = P1;
        Mode1PixelData[i].Pixel[1] = P1;
        Mode1PixelData[i].Pixel[2] = P2;
        Mode1PixelData[i].Pixel[3] = P2;
        Mode1PixelData[i].Pixel[4] = P3;
        Mode1PixelData[i].Pixel[5] = P3;
        Mode1PixelData[i].Pixel[6] = P4;
        Mode1PixelData[i].Pixel[7] = P4;
        Mode1PackedPixels[i] = PACK8_PIXELS(P1,P1,P2,P2,P3,P3,P4,P4);
        Mode1PenIndex[i]     = (unsigned long)P1;
    }

    /* ── Mode 2: 640 pixels, 2 colours ──────────────────────────────────── */
    for (i = 0; i < 256; i++) {
        int P1 = (i >> 7) & 1, P2 = (i >> 6) & 1;
        int P3 = (i >> 5) & 1, P4 = (i >> 4) & 1;
        int P5 = (i >> 3) & 1, P6 = (i >> 2) & 1;
        int P7 = (i >> 1) & 1, P8 = (i     ) & 1;
        Mode2PixelData[i].Pixel[0] = P1;
        Mode2PixelData[i].Pixel[1] = P2;
        Mode2PixelData[i].Pixel[2] = P3;
        Mode2PixelData[i].Pixel[3] = P4;
        Mode2PixelData[i].Pixel[4] = P5;
        Mode2PixelData[i].Pixel[5] = P6;
        Mode2PixelData[i].Pixel[6] = P7;
        Mode2PixelData[i].Pixel[7] = P8;
        Mode2PackedPixels[i] = PACK8_PIXELS(P1,P2,P3,P4,P5,P6,P7,P8);
        Mode2PenIndex[i]     = (unsigned long)P1;
    }

    /* ── Mode 3: undocumented (same decode as mode 0) ────────────────────── */
    for (i = 0; i < 256; i++) {
        int P1 = ((i & 0x08) >> 2) | ((i & 0x80) >> 7);
        int P2 = ((i & 0x04) >> 1) | ((i & 0x40) >> 6);
        Mode3PixelData[i].Pixel[0] = P1;
        Mode3PixelData[i].Pixel[1] = P1;
        Mode3PixelData[i].Pixel[2] = P1;
        Mode3PixelData[i].Pixel[3] = P1;
        Mode3PixelData[i].Pixel[4] = P2;
        Mode3PixelData[i].Pixel[5] = P2;
        Mode3PixelData[i].Pixel[6] = P2;
        Mode3PixelData[i].Pixel[7] = P2;
        Mode3PackedPixels[i] = PACK8_PIXELS(P1,P1,P1,P1,P2,P2,P2,P2);
        Mode3PenIndex[i]     = (unsigned long)P1;
    }
}

/* ── Free tables (called from Render_Finish) ─────────────────────────────── */

void Render_ESP32_FreeRenderTables(void)
{
    heap_caps_free(Mode0PixelData);    Mode0PixelData    = NULL;
    heap_caps_free(Mode1PixelData);    Mode1PixelData    = NULL;
    heap_caps_free(Mode2PixelData);    Mode2PixelData    = NULL;
    heap_caps_free(Mode3PixelData);    Mode3PixelData    = NULL;
    heap_caps_free(Mode0PackedPixels); Mode0PackedPixels = NULL;
    heap_caps_free(Mode1PackedPixels); Mode1PackedPixels = NULL;
    heap_caps_free(Mode2PackedPixels); Mode2PackedPixels = NULL;
    heap_caps_free(Mode3PackedPixels); Mode3PackedPixels = NULL;
    heap_caps_free(Mode0PenIndex);     Mode0PenIndex     = NULL;
    heap_caps_free(Mode1PenIndex);     Mode1PenIndex     = NULL;
    heap_caps_free(Mode2PenIndex);     Mode2PenIndex     = NULL;
    heap_caps_free(Mode3PenIndex);     Mode3PenIndex     = NULL;
}
