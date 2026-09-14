// YUV -> RGB565 for psram_video: FFmpeg's planar 8-bit pictures (4:2:0 from
// H.264, MPEG-4 and VP8; 4:2:2 and 4:4:4 from MJPEG; grey) into the RGB565
// frames the loader's PPA scales onto the panel. Integer only, 8-bit
// fixed-point coefficients, and the clamp-and-pack done with three small
// tables, so a pixel is a few adds, shifts and loads. No libswscale.
#include "papp_port.h"

#include <string.h>

typedef struct {
    int cy, yoff;           // luma gain (x256) and black level
    int crv, cgu, cgv, cbu; // chroma gains (x256)
} coeffs_t;

// ITU-R BT.601 and BT.709, TV (16-235) and full (0-255, JPEG) range.
static const coeffs_t k601_tv = {298, 16, 409, 100, 208, 516};
static const coeffs_t k709_tv = {298, 16, 459, 55, 136, 541};
static const coeffs_t k601_full = {256, 0, 359, 88, 183, 454};
static const coeffs_t k709_full = {256, 0, 403, 48, 120, 475};

// Channel value (after >> 8) -> its bits of the RGB565 word, clamped.
// Indices run from TAB_LO to TAB_LO + TAB_SIZE - 1.
#define TAB_LO (-384)
#define TAB_SIZE 1152
static uint16_t s_rtab[TAB_SIZE], s_gtab[TAB_SIZE], s_btab[TAB_SIZE];
static int s_tabs_ready = 0;

static void init_tabs(void)
{
    for (int i = 0; i < TAB_SIZE; i++) {
        int v = i + TAB_LO;
        v = v < 0 ? 0 : (v > 255 ? 255 : v);
        s_rtab[i] = (uint16_t)((v & 0xF8) << 8);
        s_gtab[i] = (uint16_t)((v & 0xFC) << 3);
        s_btab[i] = (uint16_t)(v >> 3);
    }
    s_tabs_ready = 1;
}

static inline uint16_t pack(int y, int rv, int guv, int bu)
{
    const uint16_t *r = s_rtab - TAB_LO, *g = s_gtab - TAB_LO, *b = s_btab - TAB_LO;
    return (uint16_t)(r[(y + rv) >> 8] | g[(y + guv) >> 8] | b[(y + bu) >> 8]);
}

// One row where two neighbouring luma samples share a chroma sample (4:2:0
// and 4:2:2 at full size). yr/ur/vr point at the first sample; the row's
// start column is even.
static void row_shared(uint16_t *d, const uint8_t *yr, const uint8_t *ur, const uint8_t *vr, int w,
                       const coeffs_t *k)
{
    const int cy = k->cy, yoff = k->yoff, crv = k->crv, cgu = k->cgu, cgv = k->cgv, cbu = k->cbu;
    int i = 0;
    for (; i + 1 < w; i += 2) {
        const int u = ur[i >> 1] - 128, v = vr[i >> 1] - 128;
        const int rv = crv * v, guv = -(cgu * u + cgv * v), bu = cbu * u;
        const int y0 = cy * (yr[i] - yoff) + 128;
        const int y1 = cy * (yr[i + 1] - yoff) + 128;
        d[i] = pack(y0, rv, guv, bu);
        d[i + 1] = pack(y1, rv, guv, bu);
    }
    if (i < w) {
        const int u = ur[i >> 1] - 128, v = vr[i >> 1] - 128;
        d[i] = pack(cy * (yr[i] - yoff) + 128, crv * v, -(cgu * u + cgv * v), cbu * u);
    }
}

void papp_yuv_to_rgb565(uint16_t *dst, int dst_stride, const uint8_t *const planes[3], const int strides[3],
                        enum papp_yuv_layout layout, int x0, int y0, int w, int h, int step, bool full_range,
                        bool bt709)
{
    if (!s_tabs_ready) {
        init_tabs();
    }
    const coeffs_t *k = full_range ? (bt709 ? &k709_full : &k601_full) : (bt709 ? &k709_tv : &k601_tv);
    const int cy = k->cy, yoff = k->yoff;
    const int csx = layout == PAPP_YUV444 ? 0 : 1;  // chroma subsampling shifts
    const int csy = layout == PAPP_YUV420 ? 1 : 0;
    x0 &= ~1;
    y0 &= ~1;

    for (int oy = 0; oy < h; oy++) {
        const int sy = y0 + oy * step;
        const uint8_t *yr = planes[0] + (size_t)sy * strides[0] + x0;
        uint16_t *d = dst + (size_t)oy * dst_stride;
        if (layout == PAPP_GRAY) {
            const uint16_t *r = s_rtab - TAB_LO, *g = s_gtab - TAB_LO, *b = s_btab - TAB_LO;
            for (int ox = 0; ox < w; ox++) {
                const int v = (cy * (yr[ox * step] - yoff) + 128) >> 8;
                d[ox] = (uint16_t)(r[v] | g[v] | b[v]);
            }
            continue;
        }
        const uint8_t *ur = planes[1] + (size_t)(sy >> csy) * strides[1] + (x0 >> csx);
        const uint8_t *vr = planes[2] + (size_t)(sy >> csy) * strides[2] + (x0 >> csx);
        if (step == 1 && csx == 1) {
            row_shared(d, yr, ur, vr, w, k);
            continue;
        }
        // 4:4:4, or every second pixel: each output pixel has its own chroma.
        const int crv = k->crv, cgu = k->cgu, cgv = k->cgv, cbu = k->cbu;
        const int cstep = csx ? step >> 1 : step;  // step is 1 or 2
        for (int ox = 0; ox < w; ox++) {
            const int ci = csx ? (ox * step) >> 1 : ox * cstep;
            const int u = ur[ci] - 128, v = vr[ci] - 128;
            d[ox] = pack(cy * (yr[ox * step] - yoff) + 128, crv * v, -(cgu * u + cgv * v), cbu * u);
        }
    }
}
