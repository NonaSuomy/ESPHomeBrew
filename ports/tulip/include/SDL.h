// Not SDL. tulip/shared/keyscan.c includes <SDL.h> on every non-ESP32 build
// for the modifier masks scan_ascii() tests; this port feeds keys as
// characters (papp_display.c) and only needs the names to exist.
#pragma once

#define KMOD_LSHIFT 0x0001
#define KMOD_RSHIFT 0x0002
#define KMOD_LCTRL 0x0040
#define KMOD_RCTRL 0x0080
