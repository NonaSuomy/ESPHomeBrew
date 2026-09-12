// Red Alert (Vanilla Conquer) on the ESP32-P4 PAPP loader: shared declarations
// for the port's glue code. Nothing here is part of Vanilla Conquer itself.
#pragma once

#ifndef PAPP_APP_SIDE
#define PAPP_APP_SIDE 1
#endif
// ESP-IDF builds get this C++ spelling from newlib's <sys/cdefs.h>; the loader
// header uses the C11 keyword.
#if defined(__cplusplus) && !defined(_Static_assert)
#define _Static_assert static_assert
#endif

// The loader's own ABI header: the SDK one plus the USB mouse/keyboard services.
#include "esphome/components/papp_loader/psram_app.h"

#ifdef __cplusplus
extern "C" {
#endif

// The loader's service table, set by app_entry before anything else runs.
extern const app_services_t *papp_svc;

// Where the game data lives on the card (REDALERT.MIX, MAIN.MIX, ...).
#define PAPP_RA_DATA_DIR "/sd/roms/redalert"

// Leave the game and return to the loader (exit(), abort(), a close request).
void papp_quit(int code) __attribute__((noreturn));

// True once the loader asked the app to close (on-screen X: MENU + X held).
int papp_close_requested(void);

// Release every heap block / file the game still holds (papp_syscalls.c).
void papp_free_all_memory(void);
void papp_close_all_files(void);

// Stop the sound mixer task (papp_sound.cpp); safe when it never started.
void papp_sound_shutdown(void);

// Stop the presenter task (papp_video.cpp); safe when it never started.
void papp_video_shutdown(void);

// Microseconds since boot.
long long papp_time_us(void);

// Poll the loader's inputs into the game's keyboard/mouse queue; called from
// the game's keyboard check so the game loop also yields to other tasks.
void papp_input_poll(void);

#ifdef __cplusplus
}
#endif
