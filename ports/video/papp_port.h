// video: declarations shared by the port's own files (ports/video/*.c).
// Nothing here is part of FFmpeg.
//
// Tasks (created through the loader's task_create service):
//   papp_main  app_entry: the UI (file list, overlay, input) and presenting
//              frames at the right time. Core 0, the loader's worker task.
//   vdemux     Reads the file or HTTP stream and demuxes it into the packet
//              queues; also opens the decoders. Core 0, 64 KiB PSRAM stack.
//   vvideo     Video decoding and YUV -> RGB565 into the frame ring. Core 1,
//              128 KiB PSRAM stack.
//   vaudio     Audio decoding, resampling and audio_submit; its progress is
//              the playback clock. Core 0, 64 KiB PSRAM stack.
#pragma once

// FFmpeg's sources are built with -DHAVE_AV_CONFIG_H (its internal view of
// its own headers); the port uses the public one.
#undef HAVE_AV_CONFIG_H

#ifndef PAPP_APP_SIDE
#define PAPP_APP_SIDE 1
#endif

#include "esphome/components/papp_loader/psram_app.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The loader's service table, set by app_entry before anything else runs.
extern const app_services_t *papp_svc;

// ── Clock, sleeping, logging ──────────────────────────────────────────────

int64_t papp_time_us(void);
// Sleeps at least one scheduler tick (a shorter delay_ms is only a yield).
void papp_sleep_ms(int ms);
// Sleeps a tick when the calling task has run for 200 ms without sleeping,
// so the idle task of its core (and the task watchdog) gets to run.
void papp_yield_if_due(void);
void papp_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// ── Memory ────────────────────────────────────────────────────────────────
// malloc/free/posix_memalign go through the loader (PSRAM) and are tracked, so
// everything is handed back when the app quits (the loader does not reclaim
// an app's heap). papp_alloc_aligned is for buffers the PPA reads: whole
// cache lines.

void *papp_alloc_aligned(size_t size, size_t align);
void papp_syscalls_init(void);
void papp_syscalls_deinit(void);
void papp_free_all_memory(void);

// A spinlock whose word lives in internal RAM (atomics on PSRAM are not used).
typedef struct papp_lock papp_lock_t;
papp_lock_t *papp_lock_new(void);
void papp_lock_free(papp_lock_t *lock);
void papp_lock_take(papp_lock_t *lock);
void papp_lock_give(papp_lock_t *lock);

// exit(), abort() or a failed assert on any of the app's tasks: remembered,
// that task parks, and app_entry winds everything down.
void papp_fatal(int code) __attribute__((noreturn));
extern volatile int papp_fatal_code;
extern volatile int papp_fatal_hit;

// ── Input streams (papp_io.c) ─────────────────────────────────────────────
// A file on the card (/sd/... or file:///sd/...) or an http(s):// URL, read
// through the loader's file, net_* and net_tls_* services. HTTP reads go
// through a read-ahead ring in PSRAM; seeking uses Range requests.

typedef struct papp_stream papp_stream_t;

// NULL on failure, with a one-line reason in err.
papp_stream_t *papp_stream_open(const char *location, volatile int *abort_flag, char *err, size_t err_len);
// Bytes read (> 0), 0 at the end, < 0 on an error.
int papp_stream_read(papp_stream_t *s, uint8_t *buf, int size);
// 0 when the next read starts at pos, < 0 when it cannot.
int papp_stream_seek(papp_stream_t *s, int64_t pos);
int64_t papp_stream_size(papp_stream_t *s);   // -1 when unknown
int64_t papp_stream_tell(papp_stream_t *s);
bool papp_stream_seekable(papp_stream_t *s);
// Reads what the network has ready into the read-ahead ring without waiting
// (HTTP only); the demuxer calls it while its queues are full.
void papp_stream_prefetch(papp_stream_t *s);
// How much is buffered ahead of the read position, in bytes.
int64_t papp_stream_buffered(papp_stream_t *s);
void papp_stream_close(papp_stream_t *s);
// Everything the stream layer holds (the TLS session), when the app quits.
void papp_stream_shutdown(void);

// ── YUV -> RGB565 (papp_yuv.c) ────────────────────────────────────────────

enum papp_yuv_layout { PAPP_YUV420, PAPP_YUV422, PAPP_YUV444, PAPP_GRAY };

// Converts the w x h area at (x0, y0) of an 8-bit planar picture into dst
// (stride dst_stride pixels). full_range: JPEG levels (0-255); bt709: HD
// colours. With step 2 every second pixel and row is taken (half size).
void papp_yuv_to_rgb565(uint16_t *dst, int dst_stride, const uint8_t *const planes[3], const int strides[3],
                        enum papp_yuv_layout layout, int x0, int y0, int w, int h, int step, bool full_range,
                        bool bt709);

// ── Drawing (papp_font.c) ─────────────────────────────────────────────────

typedef struct {
    uint16_t *px;
    int w, h, stride;
} papp_canvas_t;

void papp_fill(const papp_canvas_t *c, int x, int y, int w, int h, uint16_t color);
// Halves the brightness of an area (a see-through dark band).
void papp_dim(const papp_canvas_t *c, int x, int y, int w, int h);
// 8x8 font, each dot scale x scale pixels. Returns the x after the text.
int papp_text(const papp_canvas_t *c, int x, int y, const char *text, int scale, uint16_t color);
int papp_text_width(const char *text, int scale);
// Text cut with "..." to fit max_w pixels.
int papp_text_fit(const papp_canvas_t *c, int x, int y, const char *text, int scale, uint16_t color, int max_w);

#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

#ifdef __cplusplus
}
#endif
