// The fonts compiled into the app (.papp-gen/papp_fonts.c, written by
// gen_netsurf.py): subsets of the DejaVu fonts, see gen_netsurf.py FONTS.
#pragma once

#include <stddef.h>
#include <stdint.h>

struct papp_font {
    const char *name;     // the DejaVu file name, e.g. "DejaVuSans.ttf"
    const uint8_t *data;  // a TrueType font
    size_t len;
};

// Ends with a {NULL, NULL, 0} entry.
extern const struct papp_font papp_fonts[];
