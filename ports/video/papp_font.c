// Drawing for video's file list and overlay: filled and dimmed
// rectangles and text in the VGA 8x16 ROM font that FFmpeg's libavutil
// already carries (xga_font_data.c), scaled by whole pixels.
#include "papp_port.h"

#include <string.h>

extern const uint8_t avpriv_vga16_font[4096];

#define GLYPH_W 8
#define GLYPH_H 16

static bool clip(const papp_canvas_t *c, int *x, int *y, int *w, int *h)
{
    if (*x < 0) {
        *w += *x;
        *x = 0;
    }
    if (*y < 0) {
        *h += *y;
        *y = 0;
    }
    if (*x + *w > c->w) {
        *w = c->w - *x;
    }
    if (*y + *h > c->h) {
        *h = c->h - *y;
    }
    return *w > 0 && *h > 0;
}

void papp_fill(const papp_canvas_t *c, int x, int y, int w, int h, uint16_t color)
{
    if (!clip(c, &x, &y, &w, &h)) {
        return;
    }
    for (int row = 0; row < h; row++) {
        uint16_t *p = c->px + (size_t)(y + row) * c->stride + x;
        for (int i = 0; i < w; i++) {
            p[i] = color;
        }
    }
}

void papp_dim(const papp_canvas_t *c, int x, int y, int w, int h)
{
    if (!clip(c, &x, &y, &w, &h)) {
        return;
    }
    for (int row = 0; row < h; row++) {
        uint16_t *p = c->px + (size_t)(y + row) * c->stride + x;
        for (int i = 0; i < w; i++) {
            p[i] = (uint16_t)((p[i] >> 1) & 0x7BEF);  // every channel halved
        }
    }
}

static void glyph(const papp_canvas_t *c, int x, int y, unsigned char ch, int scale, uint16_t color)
{
    const uint8_t *rows = avpriv_vga16_font + (size_t)ch * GLYPH_H;
    for (int gy = 0; gy < GLYPH_H; gy++) {
        const uint8_t bits = rows[gy];
        if (bits == 0) {
            continue;
        }
        for (int gx = 0; gx < GLYPH_W; gx++) {
            if (bits & (0x80 >> gx)) {
                papp_fill(c, x + gx * scale, y + gy * scale, scale, scale, color);
            }
        }
    }
}

int papp_text_width(const char *text, int scale)
{
    return (int)strlen(text) * GLYPH_W * scale;
}

int papp_text(const papp_canvas_t *c, int x, int y, const char *text, int scale, uint16_t color)
{
    if (scale < 1) {
        scale = 1;
    }
    for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; p++) {
        if (x >= c->w) {
            break;
        }
        // Bytes of UTF-8 sequences show as '?': the font is code page 437.
        glyph(c, x, y, *p < 0x80 ? *p : '?', scale, color);
        x += GLYPH_W * scale;
    }
    return x;
}

int papp_text_fit(const papp_canvas_t *c, int x, int y, const char *text, int scale, uint16_t color, int max_w)
{
    if (scale < 1) {
        scale = 1;
    }
    const int cell = GLYPH_W * scale;
    const int fits = max_w / cell;
    const int len = (int)strlen(text);
    if (len <= fits) {
        return papp_text(c, x, y, text, scale, color);
    }
    if (fits < 4) {
        return x;
    }
    char buf[256];
    int keep = fits - 3;
    if (keep > (int)sizeof(buf) - 4) {
        keep = (int)sizeof(buf) - 4;
    }
    memcpy(buf, text, (size_t)keep);
    memcpy(buf + keep, "...", 4);
    return papp_text(c, x, y, buf, scale, color);
}
