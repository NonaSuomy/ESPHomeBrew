// NetSurf's resources compiled into the app (.papp-gen/papp_resources.c,
// written by gen_netsurf.py): what resource: URLs serve and the Messages file.
#pragma once

#include <stddef.h>
#include <stdint.h>

struct papp_resource {
    const char *name;     // resource: path, e.g. "default.css"
    const uint8_t *data;  // NUL-terminated
    size_t len;           // without the NUL
};

// Ends with a {NULL, NULL, 0} entry.
extern const struct papp_resource papp_resources[];

// English Messages for the framebuffer frontend, "key:value" lines.
extern const uint8_t *const papp_messages;
extern const size_t papp_messages_len;
