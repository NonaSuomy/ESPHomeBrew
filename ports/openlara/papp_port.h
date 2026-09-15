// OpenLara on the ESP32-P4 PAPP loader: shared declarations for the port's
// glue code. Nothing here is part of OpenLara itself.
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

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The loader's service table, set by app_entry before anything else runs.
extern const app_services_t *papp_svc;

// Game data, settings and saves: TR1 PC files (DATA/*.PHD, FMV/, ...) go in
// this folder on the card, as the original port's "data" folder. Ends in '/'
// because OpenLara prepends it to relative names.
#define PAPP_OL_DATA_DIR "/sd/roms/openlara/"

// The game renders 320 pixels wide and at most 240 high, in RGB565: 320x240
// (scaled 2x to 640x480, or 2.5x to 800x600) or, on the 640x400 canvas,
// 320x200 (papp_video_frame_height()). The loader scales it to the canvas.
#define PAPP_OL_WIDTH 320
#define PAPP_OL_HEIGHT 240

// Engine output: 44.1 kHz stereo 16-bit (Sound::fill).
#define PAPP_OL_AUDIO_RATE 44100

// Leave the game and return to the loader (exit(), abort(), out of memory).
void papp_quit(int code) __attribute__((noreturn));

// Microseconds since boot.
long long papp_time_us(void);

// Sleep one real FreeRTOS tick when the game task has not slept for a while,
// so the idle task (and its watchdog) on this core still runs. Game task only.
void papp_yield_maybe(void);

// Release every heap block / file the game still holds (papp_syscalls.c).
void papp_free_all_memory(void);
void papp_close_all_files(void);

// Heap allocation that bypasses the leak list (buffers shared with other
// tasks, freed explicitly). PSRAM unless internal is asked for.
void *papp_alloc_raw(size_t size, int internal);

// ── Video (papp_video.cpp) ────────────────────────────────────────────────
// Two RGB565 frames of PAPP_OL_WIDTH x papp_video_frame_height(): the game
// draws into papp_video_back() while a presenter task on the other core
// scales the previous one to the screen. papp_video_init picks the canvas.
int papp_video_init(void);
int papp_video_frame_height(void);  // 240, or 200 on the 640x400 canvas
uint16_t *papp_video_back(void);
void papp_video_present(void);  // hand the back buffer over; returns the next one via papp_video_back()
void papp_video_shutdown(void);

// ── Audio (papp_audio.cpp) ────────────────────────────────────────────────
// A ring of stereo frames the game task fills (Sound::fill) and a task that
// feeds it to the loader's speaker in real time.
int papp_audio_init(void);
int papp_audio_wanted(void);                    // frames to add to reach the target fill
void papp_audio_write(const int16_t *stereo, int frames);
void papp_audio_shutdown(void);

// ── Game (papp_openlara.cpp, the only file that includes OpenLara) ───────
int papp_openlara_run(void);

#ifdef __cplusplus
}
#endif
