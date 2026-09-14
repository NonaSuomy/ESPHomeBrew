// Path plotting for the NetSurf PAPP: what SVG images draw. NetSurf's
// framebuffer frontend leaves its path plotter empty ("path
// unimplemented"); patches/0010 makes it call papp_plot_path() here.
//
// A path (moves, lines, cubic Beziers, closes; content/handlers/image/svg.c
// with libsvgtiny) becomes a FreeType outline in screen pixels. Its fill is
// drawn with FreeType's anti-aliasing rasteriser (non-zero winding) and its
// stroke with FreeType's stroker, which turns the line into an outline that
// is drawn the same way. Coverage spans are blended straight into the RGB565
// canvas inside the plotter's clip rectangle.
#include "papp_port.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include FT_STROKER_H

#include "libnsfb.h"
#include "libnsfb_plot.h"

#include "utils/errors.h"
#include "utils/log.h"
#include "netsurf/plotters.h"
#include "netsurf/plot_style.h"

// papp_font.c: FreeType, set up with the fonts (NULL before that).
FT_Library papp_font_library(void);

nserror papp_plot_path(nsfb_t *nsfb, const plot_style_t *style, const float *p, unsigned int n,
                       const float transform[6]);

#define COORD_MAX 16384.0f  // pixels either way; further points are clamped

// ── The outline being built ────────────────────────────────────────────────

struct path {
    FT_Vector *points;
    unsigned char *tags;
    unsigned short *ends;  // last point of each contour
    bool *closed;          // per contour: ended with a close
    unsigned n_points, n_contours;
    unsigned cap_points, cap_contours;
    int start;             // first point of the open contour, -1 if none
    bool failed;
};

static bool grow(void **array, unsigned *cap, unsigned need, size_t item)
{
    if (need <= *cap) {
        return true;
    }
    unsigned cap2 = *cap ? *cap * 2 : 64;
    while (cap2 < need) {
        cap2 *= 2;
    }
    void *bigger = realloc(*array, (size_t)cap2 * item);
    if (bigger == NULL) {
        return false;
    }
    *array = bigger;
    *cap = cap2;
    return true;
}

static FT_Pos to_26_6(float v)
{
    if (!(v > -COORD_MAX)) {  // also catches NaN
        v = -COORD_MAX;
    } else if (v > COORD_MAX) {
        v = COORD_MAX;
    }
    return (FT_Pos)lroundf(v * 64.0f);
}

static void end_contour(struct path *pa, bool closed)
{
    if (pa->start < 0) {
        return;
    }
    unsigned cap = pa->cap_contours;  // both arrays grow to the same size
    if ((int)pa->n_points - pa->start < 2) {
        pa->n_points = (unsigned)pa->start;  // a lone point draws nothing
    } else if (!grow((void **)&pa->ends, &cap, pa->n_contours + 1, sizeof(*pa->ends)) ||
               !grow((void **)&pa->closed, &pa->cap_contours, pa->n_contours + 1, sizeof(*pa->closed))) {
        pa->failed = true;
    } else {
        pa->ends[pa->n_contours] = (unsigned short)(pa->n_points - 1);
        pa->closed[pa->n_contours] = closed;
        pa->n_contours++;
    }
    pa->start = -1;
}

static void add_point(struct path *pa, const float transform[6], float x, float y, unsigned char tag)
{
    if (pa->failed) {
        return;
    }
    if (pa->n_points >= FT_OUTLINE_POINTS_MAX - 1 || pa->n_contours >= FT_OUTLINE_CONTOURS_MAX - 1) {
        pa->failed = true;  // too big to draw
        return;
    }
    unsigned cap = pa->cap_points;  // both arrays grow to the same size
    if (!grow((void **)&pa->points, &cap, pa->n_points + 1, sizeof(*pa->points)) ||
        !grow((void **)&pa->tags, &pa->cap_points, pa->n_points + 1, sizeof(*pa->tags))) {
        pa->failed = true;
        return;
    }
    pa->points[pa->n_points].x = to_26_6(transform[0] * x + transform[2] * y + transform[4]);
    pa->points[pa->n_points].y = to_26_6(transform[1] * x + transform[3] * y + transform[5]);
    pa->tags[pa->n_points] = tag;
    pa->n_points++;
}

// NetSurf's path array: PLOTTER_PATH_MOVE x y, PLOTTER_PATH_LINE x y,
// PLOTTER_PATH_BEZIER x1 y1 x2 y2 x y, PLOTTER_PATH_CLOSE.
static bool build(struct path *pa, const float *p, unsigned int n, const float transform[6])
{
    unsigned i = 0;
    while (i < n && !pa->failed) {
        const int cmd = (int)p[i];
        if (cmd == PLOTTER_PATH_MOVE && i + 2 < n) {
            end_contour(pa, false);
            pa->start = (int)pa->n_points;
            add_point(pa, transform, p[i + 1], p[i + 2], FT_CURVE_TAG_ON);
            i += 3;
        } else if (cmd == PLOTTER_PATH_LINE && i + 2 < n) {
            if (pa->start < 0) {  // no move first: start where the last contour did
                pa->start = (int)pa->n_points;
            }
            add_point(pa, transform, p[i + 1], p[i + 2], FT_CURVE_TAG_ON);
            i += 3;
        } else if (cmd == PLOTTER_PATH_BEZIER && i + 6 < n) {
            if (pa->start < 0) {
                pa->start = (int)pa->n_points;
            }
            add_point(pa, transform, p[i + 1], p[i + 2], FT_CURVE_TAG_CUBIC);
            add_point(pa, transform, p[i + 3], p[i + 4], FT_CURVE_TAG_CUBIC);
            add_point(pa, transform, p[i + 5], p[i + 6], FT_CURVE_TAG_ON);
            i += 7;
        } else if (cmd == PLOTTER_PATH_CLOSE) {
            end_contour(pa, true);
            i += 1;
        } else {
            NSLOG(netsurf, INFO, "bad path element %d at %u of %u", cmd, i, n);
            return false;
        }
    }
    end_contour(pa, false);
    return !pa->failed;
}

// A contour must start on a curve point for FreeType. A contour that
// starts with a Bezier (a LINE/BEZIER with no MOVE) already has its first
// point as a control point; drop such contours.
static void check_starts(struct path *pa)
{
    unsigned first = 0;
    for (unsigned c = 0; c < pa->n_contours; c++) {
        if (pa->tags[first] != FT_CURVE_TAG_ON) {
            for (unsigned k = first; k <= pa->ends[c]; k++) {
                pa->tags[k] = FT_CURVE_TAG_ON;  // draw it as straight lines rather than fail
            }
        }
        first = pa->ends[c] + 1u;
    }
}

// ── Drawing ─────────────────────────────────────────────────────────────────

struct target {
    uint8_t *ptr;
    int linelen;
    int width, height;
    colour c;  // 0xBBGGRR
};

static void spans(int y, int count, const FT_Span *span, void *user)
{
    const struct target *t = user;
    if (y < 0 || y >= t->height) {
        return;
    }
    uint16_t *row = (uint16_t *)(t->ptr + y * t->linelen);
    const unsigned r = t->c & 0xFF, g = (t->c >> 8) & 0xFF, b = (t->c >> 16) & 0xFF;
    const uint16_t solid = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    for (int s = 0; s < count; s++) {
        int x = span[s].x;
        int end = x + span[s].len;
        if (end > t->width) {
            end = t->width;
        }
        const unsigned a = span[s].coverage;
        if (a == 255) {
            for (; x < end; x++) {
                row[x] = solid;
            }
            continue;
        }
        for (; x < end; x++) {
            const uint16_t px = row[x];
            unsigned pr = (px >> 8) & 0xF8, pg = (px >> 3) & 0xFC, pb = (px << 3) & 0xF8;
            pr = (r * a + pr * (255 - a)) / 255;
            pg = (g * a + pg * (255 - a)) / 255;
            pb = (b * a + pb * (255 - a)) / 255;
            row[x] = (uint16_t)(((pr & 0xF8) << 8) | ((pg & 0xFC) << 3) | (pb >> 3));
        }
    }
}

static void render(FT_Library library, FT_Outline *outline, struct target *t, const nsfb_bbox_t *clip, colour c)
{
    FT_Raster_Params params;
    memset(&params, 0, sizeof(params));
    t->c = c;
    params.flags = FT_RASTER_FLAG_AA | FT_RASTER_FLAG_DIRECT | FT_RASTER_FLAG_CLIP;
    params.gray_spans = spans;
    params.user = t;
    params.clip_box.xMin = clip->x0;
    params.clip_box.yMin = clip->y0;
    params.clip_box.xMax = clip->x1;
    params.clip_box.yMax = clip->y1;
    FT_Outline_Render(library, outline, &params);
}

static void stroke(FT_Library library, const struct path *pa, FT_Fixed radius, struct target *t,
                   const nsfb_bbox_t *clip, colour c)
{
    FT_Stroker stroker;
    if (FT_Stroker_New(library, &stroker) != 0) {
        return;
    }
    FT_Stroker_Set(stroker, radius, FT_STROKER_LINECAP_BUTT, FT_STROKER_LINEJOIN_MITER_FIXED, 4 * 0x10000);
    unsigned first = 0;
    for (unsigned ci = 0; ci < pa->n_contours; ci++) {
        const unsigned last = pa->ends[ci];
        FT_Vector *pt = pa->points;
        if (FT_Stroker_BeginSubPath(stroker, &pt[first], !pa->closed[ci]) != 0) {
            break;
        }
        for (unsigned k = first + 1; k <= last; k++) {
            if (pa->tags[k] == FT_CURVE_TAG_CUBIC && k + 2 <= last) {
                FT_Stroker_CubicTo(stroker, &pt[k], &pt[k + 1], &pt[k + 2]);
                k += 2;
            } else {
                FT_Stroker_LineTo(stroker, &pt[k]);
            }
        }
        FT_Stroker_EndSubPath(stroker);
        first = last + 1;
    }
    FT_UInt np = 0, nc = 0;
    FT_Outline outline;
    if (FT_Stroker_GetCounts(stroker, &np, &nc) == 0 && np > 0 && np < FT_OUTLINE_POINTS_MAX &&
        nc < FT_OUTLINE_CONTOURS_MAX && FT_Outline_New(library, np, (FT_Int)nc, &outline) == 0) {
        outline.n_points = 0;
        outline.n_contours = 0;
        FT_Stroker_Export(stroker, &outline);
        render(library, &outline, t, clip, c);
        FT_Outline_Done(library, &outline);
    }
    FT_Stroker_Done(stroker);
}

nserror papp_plot_path(nsfb_t *nsfb, const plot_style_t *style, const float *p, unsigned int n,
                       const float transform[6])
{
    FT_Library library = papp_font_library();
    const bool fill = style->fill_type != PLOT_OP_TYPE_NONE;
    const bool line = style->stroke_type != PLOT_OP_TYPE_NONE;
    if (library == NULL || n == 0 || (!fill && !line)) {
        return NSERROR_OK;
    }

    struct target t;
    nsfb_bbox_t clip;
    if (nsfb_get_buffer(nsfb, &t.ptr, &t.linelen) != 0 || t.ptr == NULL) {
        return NSERROR_OK;
    }
    nsfb_get_geometry(nsfb, &t.width, &t.height, NULL);
    nsfb_plot_get_clip(nsfb, &clip);
    if (clip.x0 < 0) clip.x0 = 0;
    if (clip.y0 < 0) clip.y0 = 0;
    if (clip.x1 > t.width) clip.x1 = t.width;
    if (clip.y1 > t.height) clip.y1 = t.height;
    if (clip.x0 >= clip.x1 || clip.y0 >= clip.y1) {
        return NSERROR_OK;
    }

    struct path pa;
    memset(&pa, 0, sizeof(pa));
    pa.start = -1;
    nserror result = NSERROR_OK;
    if (!build(&pa, p, n, transform)) {
        result = pa.failed ? NSERROR_NOMEM : NSERROR_INVALID;
    } else if (pa.n_contours > 0) {
        check_starts(&pa);
        if (fill) {
            FT_Outline outline;
            memset(&outline, 0, sizeof(outline));
            outline.n_points = (unsigned short)pa.n_points;
            outline.n_contours = (unsigned short)pa.n_contours;
            outline.points = pa.points;
            outline.tags = pa.tags;
            outline.contours = pa.ends;
            outline.flags = FT_OUTLINE_NONE;  // non-zero winding
            render(library, &outline, &t, &clip, style->fill_colour);
        }
        if (line) {
            // The stroke width is in the image's units: scale it as the path.
            const float scale = sqrtf(fabsf(transform[0] * transform[3] - transform[1] * transform[2]));
            float width = plot_style_fixed_to_float(style->stroke_width) * scale;
            if (width < 1.0f) {
                width = 1.0f;
            } else if (width > 1000.0f) {
                width = 1000.0f;
            }
            stroke(library, &pa, (FT_Fixed)lroundf(width * 32.0f), &t, &clip, style->stroke_colour);
        }
    }
    free(pa.points);
    free(pa.tags);
    free(pa.ends);
    free(pa.closed);
    return result;
}
