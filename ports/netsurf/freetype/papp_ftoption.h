/* FreeType's build options for the NetSurf PAPP (FT_CONFIG_OPTIONS_H in
 * apps/netsurf/papp.json): FreeType's defaults, minus what the port
 * does not use. NetSurf draws TrueType outlines anti-aliased with the
 * smooth renderer and hints them with the autofitter; the fonts come from
 * memory (the DejaVu subsets compiled in) or from files on the card. */
#ifndef PAPP_FTOPTION_H
#define PAPP_FTOPTION_H

#include <freetype/config/ftoption.h>

/* No compressed (gzip, LZW), Mac, incremental or SVG-in-OpenType fonts,
 * no PostScript glyph names and no FREETYPE_PROPERTIES variable. */
#undef FT_CONFIG_OPTION_USE_LZW
#undef FT_CONFIG_OPTION_USE_ZLIB
#undef FT_CONFIG_OPTION_POSTSCRIPT_NAMES
#undef FT_CONFIG_OPTION_ADOBE_GLYPH_LIST
#undef FT_CONFIG_OPTION_MAC_FONTS
#undef FT_CONFIG_OPTION_GUESSING_EMBEDDED_RFORK
#undef FT_CONFIG_OPTION_INCREMENTAL
#undef FT_CONFIG_OPTION_SVG
#undef FT_CONFIG_OPTION_ENVIRONMENT_PROPERTIES

/* TrueType outlines only: no embedded bitmaps, colour layers, variation
 * fonts, BDF properties or glyph names, and no bytecode interpreter (the
 * autofitter hints instead, as NetSurf asks with FT_LOAD_FORCE_AUTOHINT). */
#undef TT_CONFIG_OPTION_EMBEDDED_BITMAPS
#undef TT_CONFIG_OPTION_COLOR_LAYERS
#undef TT_SUPPORT_COLRV1
#undef TT_CONFIG_OPTION_POSTSCRIPT_NAMES
#undef TT_CONFIG_OPTION_BYTECODE_INTERPRETER
#undef TT_CONFIG_OPTION_SUBPIXEL_HINTING
#undef TT_USE_BYTECODE_INTERPRETER
#undef TT_SUPPORT_SUBPIXEL_HINTING_MINIMAL
#undef TT_CONFIG_OPTION_GX_VAR_SUPPORT
#undef TT_CONFIG_OPTION_BDF

/* The autofitter's Latin, Greek and Cyrillic scripts only. */
#undef AF_CONFIG_OPTION_CJK
#undef AF_CONFIG_OPTION_INDIC

#endif
