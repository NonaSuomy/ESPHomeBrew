// Tulip Creative Computer on the ESP32-P4 PAPP loader: declarations shared by
// the port's glue code (ports/tulip/*.c). Nothing here is part of Tulip.
//
// Tasks (all created through the loader's task_create service):
//   mp       MicroPython: _boot.py, boot.py, the REPL. Core 0, PSRAM stack.
//   display  Tulip's compositor (display_bounce_empty) into the loader's
//            800x480 RGB565 canvas, ~30 fps, plus input polling. Core 1.
//   audio    AMY rendering (amy_simple_fill_buffer) into the loader's
//            speaker at 44.1 kHz. Core 1.
#pragma once

#ifndef PAPP_APP_SIDE
#define PAPP_APP_SIDE 1
#endif

#include "esphome/components/papp_loader/psram_app.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The loader's service table, set by app_entry before anything else runs.
extern const app_services_t *papp_svc;

// Tulip's files live here on the card: tulip.lfs (the Tulip filesystem image
// mounted at /), and anything the user copies for import from /sd.
#define PAPP_TULIP_DIR "/sd/roms/tulip"

// Leave Tulip and return to the loader (exit(), abort(), fatal errors, the
// close control). Safe from any of the app's tasks.
void papp_quit(int code) __attribute__((noreturn));
void papp_request_quit(int code);  // same, but returns (display/input code)
volatile int *papp_quit_flag(void);

// Microseconds since boot.
int64_t papp_time_us(void);

// PSRAM (or internal RAM) straight from the loader, not tracked by the
// app heap's leak list; freed with papp_free_raw.
void *papp_alloc_raw(size_t size, int internal);
void papp_free_raw(void *ptr);

// Heap and files the app still holds, released when it quits.
void papp_syscalls_init(void);
void papp_syscalls_deinit(void);
void papp_free_all_memory(void);
void papp_close_all_files(void);

// Loader files (FILE * behind the service table), remembered so they can be
// closed when Tulip quits.
void *papp_file_open(const char *path, const char *mode);
int papp_file_close(void *fp);
long papp_file_size(void *fp);

// A spinlock usable across the app's tasks and cores (the loader has no mutex
// service). Lock words live in internal RAM.
typedef struct papp_lock papp_lock_t;
papp_lock_t *papp_lock_new(void);
void papp_lock_take(papp_lock_t *lock);
void papp_lock_give(papp_lock_t *lock);

// MicroPython's atomic sections (scheduler queue, keyboard interrupt) and
// AMY's event queue lock.
unsigned papp_atomic_begin(void);
void papp_atomic_end(unsigned state);
void papp_amy_lock(int grab);

// ── Display and input (papp_display.c) ──────────────────────────────────
int papp_display_start(void);
void papp_display_stop(void);

// ── Audio (papp_audio.c) ─────────────────────────────────────────────────
void run_amy(void);
void papp_audio_stop(void);

// ── MicroPython (papp_main.c) ────────────────────────────────────────────
// Set once the MicroPython task has its heap and scheduler (input waits for it).
extern volatile int papp_mp_ready;

#ifdef __cplusplus
}
#endif
