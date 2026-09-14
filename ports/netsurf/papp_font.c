/*
 * Text for the NetSurf PAPP: NetSurf's framebuffer FreeType font backend
 * (frontends/framebuffer/font_freetype.c, which this replaces in the build)
 * with the fonts this port has.
 *
 * Copyright 2005 James Bursa <bursa@users.sourceforge.net>
 *           2008 Vincent Sanders <vince@simtec.co.uk>
 * PAPP changes: the PAPP port of NetSurf.
 *
 * This file is part of NetSurf, http://www.netsurf-browser.org/
 *
 * NetSurf is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * NetSurf is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

// What differs from font_freetype.c:
//
// - Faces: sans-serif, serif and monospace, each regular, bold, italic and
//   bold italic. Each comes from the first of: the file a Choices option
//   names (fb_face_*), the DejaVu file of that name in PAPP_NS_DIR/fonts/
//   (DejaVuSans.ttf, DejaVuSerif-Italic.ttf, ...), the DejaVu subset
//   compiled into the app (regular and bold only, see gen_netsurf.py). An
//   italic face without a file is the upright one slanted. Cursive and
//   fantasy use the fb_face_cursive/fb_face_fantasy files, else sans-serif.
// - Families: the page's font-family names pick the face before the generic
//   family does, so "Georgia" alone is serif and "Courier New" monospace
//   (s_names below, then a few words such as "mono" or "serif" in a name).
// - Bold from weight 600 (semibold) up; font_freetype.c needs 700.
// - Glyphs are hinted by the autofitter (FreeType is built without the
//   TrueType bytecode interpreter), light hinting, always anti-aliased (the
//   fb_font_monochrome option is ignored: no monochrome rasteriser).
// - Zero-width characters (soft hyphen, ZWSP, joiners, BOM) draw nothing.
// - A character without a glyph no longer stalls position()/split().
// - papp_font_library() gives the FreeType library to papp_path.c.
#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_GLYPH_H
#include FT_CACHE_H

#include <libwapcaplet/libwapcaplet.h>

#include "utils/errors.h"
#include "utils/log.h"
#include "utils/nsoption.h"
#include "utils/utf8.h"
#include "netsurf/browser.h"
#include "netsurf/layout.h"
#include "netsurf/plot_style.h"

#include "framebuffer/gui.h"
#include "framebuffer/font.h"

#include "papp_port.h"
#include "papp_fonts.h"

#define CACHE_MIN_SIZE (100 * 1024)  // glyph cache, at least
#define MAX_FACES 16                 // open faces (every face of the table fits)
#define MAX_SIZES 32                 // face and size pairs kept set up
#define BOLD_WEIGHT 600
// Synthetic italic: x += 0.2126 y (12 degrees), as FT_GlyphSlot_Oblique.
#define SLANT 0x0366A

int ft_load_type = 0;

static FT_Library library;
static FTC_Manager ft_cmanager;
static FTC_CMapCache ft_cmap_cache;
static FTC_ImageCache ft_image_cache;
static bool s_ready = false;

typedef struct fb_faceid_s {
    char *fontfile;        // a font file, or NULL for compiled-in data
    const uint8_t *data;   // compiled-in font
    size_t len;
    bool slant;            // draw it slanted: an italic made from an upright face
    int cidx;              // the Unicode charmap's index
} fb_faceid_t;

enum { FAM_SANS, FAM_SERIF, FAM_MONO, FAM_CURSIVE, FAM_FANTASY, FAM_COUNT };
enum { ST_REGULAR, ST_BOLD, ST_ITALIC, ST_BOLD_ITALIC, ST_COUNT };

static fb_faceid_t *s_faces[FAM_COUNT][ST_COUNT];

// The DejaVu file for each face (on the card in PAPP_NS_DIR/fonts/, or
// compiled in under the same name).
static const char *const s_files[3][ST_COUNT] = {
    {"DejaVuSans.ttf", "DejaVuSans-Bold.ttf", "DejaVuSans-Oblique.ttf", "DejaVuSans-BoldOblique.ttf"},
    {"DejaVuSerif.ttf", "DejaVuSerif-Bold.ttf", "DejaVuSerif-Italic.ttf", "DejaVuSerif-BoldItalic.ttf"},
    {"DejaVuSansMono.ttf", "DejaVuSansMono-Bold.ttf", "DejaVuSansMono-Oblique.ttf",
     "DejaVuSansMono-BoldOblique.ttf"},
};

static FT_Error ft_face_requester(FTC_FaceID face_id, FT_Library lib, FT_Pointer request_data, FT_Face *face)
{
    (void)request_data;
    fb_faceid_t *fb_face = (fb_faceid_t *)face_id;
    FT_Error error;

    if (fb_face->fontfile != NULL) {
        error = FT_New_Face(lib, fb_face->fontfile, 0, face);
    } else {
        error = FT_New_Memory_Face(lib, fb_face->data, (FT_Long)fb_face->len, 0, face);
    }
    if (error) {
        NSLOG(netsurf, INFO, "Could not load font %s (code %d)",
              fb_face->fontfile != NULL ? fb_face->fontfile : "(built in)", error);
        return error;
    }
    error = FT_Select_Charmap(*face, FT_ENCODING_UNICODE);
    if (error) {
        NSLOG(netsurf, INFO, "Could not select charmap (code %d)", error);
        FT_Done_Face(*face);
        *face = NULL;
        return error;
    }
    for (int cidx = 0; cidx < (*face)->num_charmaps; cidx++) {
        if ((*face)->charmap == (*face)->charmaps[cidx]) {
            fb_face->cidx = cidx;
            break;
        }
    }
    if (fb_face->slant) {
        FT_Matrix shear = {0x10000, SLANT, 0, 0x10000};
        FT_Set_Transform(*face, &shear, NULL);
    }
    return 0;
}

// A face from a file or from memory, checked by loading it once.
static fb_faceid_t *new_face(const char *fontfile, const uint8_t *data, size_t len, bool slant)
{
    FT_Face aface;
    fb_faceid_t *f = calloc(1, sizeof(*f));
    if (f == NULL) {
        return NULL;
    }
    if (fontfile != NULL) {
        f->fontfile = strdup(fontfile);
        if (f->fontfile == NULL) {
            free(f);
            return NULL;
        }
    }
    f->data = data;
    f->len = len;
    f->slant = slant;
    if (FTC_Manager_LookupFace(ft_cmanager, (FTC_FaceID)f, &aface) != 0) {
        free(f->fontfile);
        free(f);
        return NULL;
    }
    return f;
}

static const struct papp_font *builtin(const char *name)
{
    for (const struct papp_font *font = papp_fonts; font->name != NULL; font++) {
        if (strcmp(font->name, name) == 0) {
            return font;
        }
    }
    return NULL;
}

// A face from the Choices option, the card or the app, in that order.
static fb_faceid_t *find_face(const char *option, const char *name)
{
    fb_faceid_t *f = NULL;
    if (option != NULL && option[0] != '\0') {
        f = new_face(option, NULL, 0, false);
        if (f == NULL) {
            papp_svc->log_printf("NETSURF: font %s (Choices) did not load\n", option);
        }
    }
    if (f == NULL && name != NULL) {
        char path[128];
        snprintf(path, sizeof(path), PAPP_NS_DIR "/fonts/%s", name);
        if (papp_file_exists(path)) {
            f = new_face(path, NULL, 0, false);
            papp_svc->log_printf("NETSURF: font %s %s\n", path, f != NULL ? "from the card" : "did not load");
        }
    }
    if (f == NULL && name != NULL) {
        const struct papp_font *font = builtin(name);
        if (font != NULL) {
            f = new_face(NULL, font->data, font->len, false);
        }
    }
    return f;
}

// The same font as `upright`, slanted.
static fb_faceid_t *slanted(const fb_faceid_t *upright)
{
    if (upright == NULL) {
        return NULL;
    }
    return new_face(upright->fontfile, upright->data, upright->len, true);
}

static void free_faces(void)
{
    fb_faceid_t **all = &s_faces[0][0];
    const int count = FAM_COUNT * ST_COUNT;
    for (int i = 0; i < count; i++) {
        if (all[i] == NULL) {
            continue;
        }
        for (int j = i + 1; j < count; j++) {
            if (all[j] == all[i]) {
                all[j] = NULL;
            }
        }
        free(all[i]->fontfile);
        free(all[i]);
        all[i] = NULL;
    }
}

/* exported interface documented in framebuffer/font.h */
bool fb_font_init(void)
{
    FT_Error error = FT_Init_FreeType(&library);
    if (error) {
        NSLOG(netsurf, INFO, "Freetype could not initialised (code %d)", error);
        return false;
    }
    FT_ULong max_cache_size = (FT_ULong)nsoption_int(fb_font_cachesize) * 1024;
    if (max_cache_size < CACHE_MIN_SIZE) {
        max_cache_size = CACHE_MIN_SIZE;
    }
    error = FTC_Manager_New(library, MAX_FACES, MAX_SIZES, max_cache_size, ft_face_requester, NULL, &ft_cmanager);
    if (error) {
        NSLOG(netsurf, INFO, "Freetype could not initialise cache manager (code %d)", error);
        FT_Done_FreeType(library);
        return false;
    }
    if (FTC_CMapCache_New(ft_cmanager, &ft_cmap_cache) != 0 ||
        FTC_ImageCache_New(ft_cmanager, &ft_image_cache) != 0) {
        FTC_Manager_Done(ft_cmanager);
        FT_Done_FreeType(library);
        return false;
    }

    // Which Choices option names a file for which face (NULL: none).
    const char *const options[3][ST_COUNT] = {
        {nsoption_charp(fb_face_sans_serif), nsoption_charp(fb_face_sans_serif_bold),
         nsoption_charp(fb_face_sans_serif_italic), nsoption_charp(fb_face_sans_serif_italic_bold)},
        {nsoption_charp(fb_face_serif), nsoption_charp(fb_face_serif_bold), NULL, NULL},
        {nsoption_charp(fb_face_monospace), nsoption_charp(fb_face_monospace_bold), NULL, NULL},
    };
    for (int fam = FAM_SANS; fam <= FAM_MONO; fam++) {
        fb_faceid_t **st = s_faces[fam];
        st[ST_REGULAR] = find_face(options[fam][ST_REGULAR], s_files[fam][ST_REGULAR]);
        if (st[ST_REGULAR] == NULL) {
            if (fam == FAM_SANS) {
                NSLOG(netsurf, INFO, "Could not find the default font");
                papp_svc->log_printf("NETSURF: no sans-serif font\n");
                free_faces();
                FTC_Manager_Done(ft_cmanager);
                FT_Done_FreeType(library);
                return false;
            }
            memcpy(st, s_faces[FAM_SANS], sizeof(s_faces[FAM_SANS]));
            continue;
        }
        st[ST_BOLD] = find_face(options[fam][ST_BOLD], s_files[fam][ST_BOLD]);
        if (st[ST_BOLD] == NULL) {
            st[ST_BOLD] = st[ST_REGULAR];
        }
        st[ST_ITALIC] = find_face(options[fam][ST_ITALIC], s_files[fam][ST_ITALIC]);
        if (st[ST_ITALIC] == NULL) {
            st[ST_ITALIC] = slanted(st[ST_REGULAR]);
        }
        st[ST_BOLD_ITALIC] = find_face(options[fam][ST_BOLD_ITALIC], s_files[fam][ST_BOLD_ITALIC]);
        if (st[ST_BOLD_ITALIC] == NULL) {
            st[ST_BOLD_ITALIC] = st[ST_BOLD] != st[ST_REGULAR] ? slanted(st[ST_BOLD]) : st[ST_ITALIC];
        }
        for (int s = ST_ITALIC; s <= ST_BOLD_ITALIC; s++) {
            if (st[s] == NULL) {
                st[s] = st[s - ST_ITALIC];  // slanting failed: upright
            }
        }
    }
    // Cursive and fantasy: only a file named in Choices, else sans-serif.
    const char *const other[2] = {nsoption_charp(fb_face_cursive), nsoption_charp(fb_face_fantasy)};
    for (int i = 0; i < 2; i++) {
        fb_faceid_t *f = find_face(other[i], NULL);
        for (int s = 0; s < ST_COUNT; s++) {
            s_faces[FAM_CURSIVE + i][s] = f != NULL ? f : s_faces[FAM_SANS][s];
        }
    }

    ft_load_type = 0;
    s_ready = true;
    return true;
}

/* exported interface documented in framebuffer/font.h */
bool fb_font_finalise(void)
{
    if (!s_ready) {
        return true;
    }
    s_ready = false;
    FTC_Manager_Done(ft_cmanager);
    FT_Done_FreeType(library);
    free_faces();
    return true;
}

FT_Library papp_font_library(void)
{
    return s_ready ? library : NULL;
}

// ── Font families ───────────────────────────────────────────────────────────

// Common font-family names and the face that stands in for them. Names
// with "mono", "courier" or "code" in them are monospace, then "sans" is
// sans-serif and "serif" serif (family_of_name).
static const struct {
    const char *name;
    int family;
} s_names[] = {
    {"arial", FAM_SANS}, {"helvetica", FAM_SANS}, {"verdana", FAM_SANS}, {"tahoma", FAM_SANS},
    {"trebuchet ms", FAM_SANS}, {"segoe ui", FAM_SANS}, {"roboto", FAM_SANS}, {"lato", FAM_SANS},
    {"calibri", FAM_SANS}, {"candara", FAM_SANS}, {"ubuntu", FAM_SANS}, {"geneva", FAM_SANS},
    {"lucida grande", FAM_SANS}, {"system-ui", FAM_SANS}, {"-apple-system", FAM_SANS},
    {"blinkmacsystemfont", FAM_SANS}, {"inter", FAM_SANS}, {"montserrat", FAM_SANS}, {"futura", FAM_SANS},
    {"avenir", FAM_SANS}, {"frutiger", FAM_SANS}, {"univers", FAM_SANS}, {"myriad pro", FAM_SANS},
    {"franklin gothic medium", FAM_SANS}, {"century gothic", FAM_SANS}, {"poppins", FAM_SANS},
    {"raleway", FAM_SANS}, {"nunito", FAM_SANS}, {"oswald", FAM_SANS}, {"arimo", FAM_SANS},
    {"cantarell", FAM_SANS}, {"optima", FAM_SANS}, {"impact", FAM_SANS},
    {"times", FAM_SERIF}, {"times new roman", FAM_SERIF}, {"georgia", FAM_SERIF}, {"garamond", FAM_SERIF},
    {"palatino", FAM_SERIF}, {"palatino linotype", FAM_SERIF}, {"book antiqua", FAM_SERIF},
    {"cambria", FAM_SERIF}, {"constantia", FAM_SERIF}, {"baskerville", FAM_SERIF}, {"bookman", FAM_SERIF},
    {"bookman old style", FAM_SERIF}, {"century schoolbook", FAM_SERIF}, {"didot", FAM_SERIF},
    {"bodoni mt", FAM_SERIF}, {"merriweather", FAM_SERIF}, {"lora", FAM_SERIF}, {"charter", FAM_SERIF},
    {"hoefler text", FAM_SERIF}, {"minion pro", FAM_SERIF}, {"tinos", FAM_SERIF}, {"lucida bright", FAM_SERIF},
    {"playfair display", FAM_SERIF}, {"linux libertine", FAM_SERIF},
    {"monaco", FAM_MONO}, {"menlo", FAM_MONO}, {"consolas", FAM_MONO}, {"lucida console", FAM_MONO},
    {"inconsolata", FAM_MONO}, {"cousine", FAM_MONO}, {"sfmono-regular", FAM_MONO},
};

static int family_of_name(lwc_string *name)
{
    char low[48];
    const size_t len = lwc_string_length(name);
    const char *text = lwc_string_data(name);
    if (len == 0 || len >= sizeof(low)) {
        return -1;
    }
    for (size_t i = 0; i < len; i++) {
        low[i] = (char)tolower((unsigned char)text[i]);
    }
    low[len] = '\0';
    for (size_t i = 0; i < sizeof(s_names) / sizeof(s_names[0]); i++) {
        if (strcmp(low, s_names[i].name) == 0) {
            return s_names[i].family;
        }
    }
    if (strstr(low, "mono") != NULL || strstr(low, "courier") != NULL || strstr(low, "code") != NULL) {
        return FAM_MONO;
    }
    if (strstr(low, "sans") != NULL) {
        return FAM_SANS;
    }
    if (strstr(low, "serif") != NULL || strstr(low, "times") != NULL) {
        return FAM_SERIF;
    }
    return -1;
}

static int family_of_generic(plot_font_generic_family_t generic)
{
    switch (generic) {
    case PLOT_FONT_FAMILY_SERIF:
        return FAM_SERIF;
    case PLOT_FONT_FAMILY_MONOSPACE:
        return FAM_MONO;
    case PLOT_FONT_FAMILY_CURSIVE:
        return FAM_CURSIVE;
    case PLOT_FONT_FAMILY_FANTASY:
        return FAM_FANTASY;
    case PLOT_FONT_FAMILY_SANS_SERIF:
    default:
        return FAM_SANS;
    }
}

// The family for a style: its first known name, else its generic family.
// Text runs share a style, so the last answer is kept.
static int family_of(const plot_font_style_t *fstyle)
{
    static struct {
        lwc_string *const *families;
        lwc_string *first;
        plot_font_generic_family_t generic;
        int family;
    } last = {NULL, NULL, PLOT_FONT_FAMILY_SANS_SERIF, FAM_SANS};

    if (fstyle->families == NULL) {
        return family_of_generic(fstyle->family);
    }
    if (last.families == fstyle->families && last.first == fstyle->families[0] && last.generic == fstyle->family) {
        return last.family;
    }
    int family = -1;
    for (lwc_string *const *name = fstyle->families; *name != NULL && family < 0; name++) {
        family = family_of_name(*name);
    }
    if (family < 0) {
        family = family_of_generic(fstyle->family);
    }
    last.families = fstyle->families;
    last.first = fstyle->families[0];
    last.generic = fstyle->family;
    last.family = family;
    return family;
}

static void fb_fill_scalar(const plot_font_style_t *fstyle, FTC_Scaler srec)
{
    const bool bold = fstyle->weight >= BOLD_WEIGHT;
    const bool italic = (fstyle->flags & (FONTF_ITALIC | FONTF_OBLIQUE)) != 0;
    const int style = italic ? (bold ? ST_BOLD_ITALIC : ST_ITALIC) : (bold ? ST_BOLD : ST_REGULAR);

    srec->face_id = (FTC_FaceID)s_faces[family_of(fstyle)][style];
    srec->width = srec->height = (fstyle->size * 64) / PLOT_STYLE_SCALE;
    srec->pixel = 0;
    srec->x_res = srec->y_res = browser_get_dpi();
}

// Characters that take no space and show nothing.
static bool zero_width(uint32_t ucs4)
{
    return ucs4 == 0x00AD || (ucs4 >= 0x200B && ucs4 <= 0x200F) || (ucs4 >= 0x2028 && ucs4 <= 0x202E) ||
           (ucs4 >= 0x2060 && ucs4 <= 0x2064) || ucs4 == 0xFEFF;
}

/* exported interface documented in framebuffer/font_freetype.h */
FT_Glyph fb_getglyph(const plot_font_style_t *fstyle, uint32_t ucs4)
{
    FTC_ScalerRec srec;
    FT_Glyph glyph;

    if (!s_ready || zero_width(ucs4)) {
        return NULL;
    }
    fb_fill_scalar(fstyle, &srec);
    const fb_faceid_t *fb_face = (const fb_faceid_t *)srec.face_id;
    const FT_UInt glyph_index = FTC_CMapCache_Lookup(ft_cmap_cache, srec.face_id, fb_face->cidx, ucs4);
    if (FTC_ImageCache_LookupScaler(ft_image_cache, &srec,
                                    FT_LOAD_RENDER | FT_LOAD_FORCE_AUTOHINT | FT_LOAD_TARGET_LIGHT | ft_load_type,
                                    glyph_index, &glyph, NULL) != 0) {
        return NULL;
    }
    return glyph;
}

static int advance(const plot_font_style_t *fstyle, uint32_t ucs4)
{
    FT_Glyph glyph = fb_getglyph(fstyle, ucs4);
    return glyph != NULL ? (int)(glyph->advance.x >> 16) : 0;
}

/* exported interface documented in framebuffer/font.h */
nserror fb_font_width(const plot_font_style_t *fstyle, const char *string, size_t length, int *width)
{
    size_t nxtchr = 0;
    *width = 0;
    while (nxtchr < length) {
        const uint32_t ucs4 = utf8_to_ucs4(string + nxtchr, length - nxtchr);
        nxtchr = utf8_next(string, length, nxtchr);
        *width += advance(fstyle, ucs4);
    }
    return NSERROR_OK;
}

/* exported interface documented in framebuffer/font.h */
nserror fb_font_position(const plot_font_style_t *fstyle, const char *string, size_t length, int x,
                         size_t *char_offset, int *actual_x)
{
    size_t nxtchr = 0;
    int prev_x = 0;
    *actual_x = 0;
    while (nxtchr < length) {
        const uint32_t ucs4 = utf8_to_ucs4(string + nxtchr, length - nxtchr);
        *actual_x += advance(fstyle, ucs4);
        if (*actual_x > x) {
            break;
        }
        prev_x = *actual_x;
        nxtchr = utf8_next(string, length, nxtchr);
    }
    // the nearer of the character's two edges
    if (abs(*actual_x - x) > abs(prev_x - x)) {
        *actual_x = prev_x;
    } else if (nxtchr < length) {
        nxtchr = utf8_next(string, length, nxtchr);
    }
    *char_offset = nxtchr;
    return NSERROR_OK;
}

/**
 * Find where to split a string to make it fit a width.
 *
 * On exit, char_offset indicates first character after split point: the
 * last space before the text passes x, else the first space after it, else
 * length (no split possible). actual_x is the width before char_offset.
 */
static nserror fb_font_split(const plot_font_style_t *fstyle, const char *string, size_t length, int x,
                             size_t *char_offset, int *actual_x)
{
    size_t nxtchr = 0;
    int last_space_x = 0;
    size_t last_space_idx = 0;

    *actual_x = 0;
    while (nxtchr < length) {
        const uint32_t ucs4 = utf8_to_ucs4(string + nxtchr, length - nxtchr);
        if (ucs4 == 0x20) {
            last_space_x = *actual_x;
            last_space_idx = nxtchr;
        }
        *actual_x += advance(fstyle, ucs4);
        if (*actual_x > x && last_space_idx != 0) {
            // past the width with a space seen: split at that space
            *actual_x = last_space_x;
            *char_offset = last_space_idx;
            return NSERROR_OK;
        }
        nxtchr = utf8_next(string, length, nxtchr);
    }
    *char_offset = nxtchr;
    return NSERROR_OK;
}

static struct gui_layout_table layout_table = {
    .width = fb_font_width,
    .position = fb_font_position,
    .split = fb_font_split,
};

struct gui_layout_table *framebuffer_layout_table = &layout_table;

struct gui_utf8_table *framebuffer_utf8_table = NULL;
