/*
 * PSRAM App Loader — Shared ABI Header
 *
 * This header defines the interface between the launcher and PSRAM-loaded apps.
 * It is used by both:
 *   - The launcher (to populate the service table and load/run apps)
 *   - PSRAM apps (to access launcher services via function pointers)
 *
 * ABI version must match between launcher and apps.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── .papp Binary Format ─────────────────────────────────────────────── */

#define PAPP_MAGIC       0x50415050   /* "PAPP" in little-endian */
#define PAPP_ABI_VERSION 1
#define PAPP_HEADER_SIZE 32
/* Zeroed pointer slots a loader leaves after app_services_t (see the
 * APPEND-ONLY ZONE), so services appended later read NULL. */
#define PAPP_SERVICES_SPARE_SLOTS 64

typedef struct __attribute__((packed)) {
    uint32_t magic;        /* Must be PAPP_MAGIC                        */
    uint32_t version;      /* ABI version (PAPP_ABI_VERSION)            */
    uint32_t entry_off;    /* Byte offset to entry fn from text start   */
    uint32_t text_size;    /* Size of .text + .rodata segment           */
    uint32_t data_size;    /* Size of .data segment (initialized)       */
    uint32_t bss_size;     /* Size of .bss segment (zero-filled)        */
    uint32_t flags;        /* Reserved, must be 0                       */
    uint32_t reserved;     /* Padding to 32 bytes                       */
} papp_header_t;

_Static_assert(sizeof(papp_header_t) == PAPP_HEADER_SIZE,
               "papp_header_t must be 32 bytes");

/* ── Gamepad State (matches odroid_gamepad_state layout) ─────────────── */

enum {
    PAPP_INPUT_UP = 0,
    PAPP_INPUT_RIGHT,
    PAPP_INPUT_DOWN,
    PAPP_INPUT_LEFT,
    PAPP_INPUT_SELECT,
    PAPP_INPUT_START,
    PAPP_INPUT_A,
    PAPP_INPUT_B,
    PAPP_INPUT_X,
    PAPP_INPUT_Y,
    PAPP_INPUT_L,
    PAPP_INPUT_R,
    PAPP_INPUT_MENU,
    PAPP_INPUT_VOLUME,
    PAPP_INPUT_MAX
};

typedef struct {
    int values[PAPP_INPUT_MAX];
} papp_gamepad_state_t;

/* A queued keyboard event from the launcher's USB HID text path.  PAPP apps
 * receive Quake-style key numbers (ASCII for printable keys, or the named
 * constants defined by the app) and a press/release state. */
typedef struct {
    int key;
    int down;
} papp_keyboard_event_t;

/* One finger on the touch panel (touch_read_points), in canvas coordinates
 * like touch_read. id stays the same while that finger stays down. */
typedef struct {
    int16_t x;
    int16_t y;
    uint8_t id;
} papp_touch_point_t;

/* ── Memory Capability Flags (matches ESP-IDF MALLOC_CAP_*) ─────────── */

#define PAPP_MEM_CAP_SPIRAM   (1 << 10)  /* MALLOC_CAP_SPIRAM */
#define PAPP_MEM_CAP_INTERNAL (1 << 11)  /* MALLOC_CAP_INTERNAL */
#define PAPP_MEM_CAP_DMA      (1 << 2)   /* MALLOC_CAP_DMA */

/* ── App Services Table ──────────────────────────────────────────────── */
/*
 * Function pointer table populated by the launcher.
 * Passed to the loaded app's entry function.
 * The PSRAM app must NOT call any function outside this table.
 */
typedef struct {
    uint32_t abi_version;   /* Must equal PAPP_ABI_VERSION */

    /* ── Display ─────────────────────────────────────────────────────── */
    uint16_t *(*display_get_framebuffer)(void);
    uint16_t *(*display_get_emu_buffer)(void);
    void      (*display_flush)(void);
    void      (*display_emu_flush)(void);
    void      (*display_clear)(uint16_t color);
    void      (*display_set_scale)(float sx, float sy);
    void      (*display_write_frame_rgb565)(const uint16_t *buffer);
    void      (*display_write_frame_custom)(const uint16_t *buffer,
                  uint16_t in_w, uint16_t in_h, float scale,
                  bool byte_swap);
    void      (*display_write_rect)(int x, int y, int w, int h,
                  const uint16_t *data);
    void      (*display_lock)(void);
    void      (*display_unlock)(void);

    /* ── Audio ───────────────────────────────────────────────────────── */
    void (*audio_init)(int sample_rate);
    void (*audio_submit)(short *stereo_buf, int frame_count);

    /* ── Input ───────────────────────────────────────────────────────── */
    void (*input_gamepad_read)(papp_gamepad_state_t *state);

    /* ── File I/O (standard C wrappers) ──────────────────────────────── */
    void  *(*file_open)(const char *path, const char *mode);
    int    (*file_close)(void *stream);
    size_t (*file_read)(void *ptr, size_t size, size_t nmemb, void *stream);
    size_t (*file_write)(const void *ptr, size_t size, size_t nmemb,
                         void *stream);
    int    (*file_seek)(void *stream, long offset, int whence);
    long   (*file_tell)(void *stream);

    /* ── Memory ──────────────────────────────────────────────────────── */
    void *(*mem_alloc)(size_t size);
    void *(*mem_calloc)(size_t n, size_t size);
    void *(*mem_realloc)(void *ptr, size_t size);
    void  (*mem_free)(void *ptr);
    void *(*mem_caps_alloc)(size_t size, uint32_t caps);

    /* ── System ──────────────────────────────────────────────────────── */
    int     (*log_printf)(const char *fmt, ...);
    int     (*log_vprintf)(const char *fmt, va_list args);
    void    (*delay_ms)(int ms);
    int64_t (*get_time_us)(void);

    /* ── Settings (NVS) ──────────────────────────────────────────────── */
    char   *(*settings_rom_path_get)(void);
    void    (*settings_rom_path_set)(const char *path);
    int32_t (*settings_volume_get)(void);
    void    (*settings_volume_set)(int32_t level);
    int32_t (*settings_brightness_get)(void);
    void    (*settings_brightness_set)(int32_t level);

    /* ── FreeRTOS Tasks ──────────────────────────────────────────────── */
    int  (*task_create)(void (*fn)(void *), const char *name,
                        uint32_t stack_depth, void *arg,
                        int priority, void *out_handle, int core);
    void (*task_delete)(void *handle);

    /* ── PNG Loading ─────────────────────────────────────────────────── */
    uint16_t *(*png_load_rgb565)(const char *path,
                                  uint16_t *out_w, uint16_t *out_h);

    /* ── PPA Sprite Blit (hardware color-keyed blend) ────────────────── */
    int (*sprite_blit)(uint16_t *framebuf, uint32_t fb_w, uint32_t fb_h,
                       uint32_t x, uint32_t y,
                       const uint16_t *sprite, uint32_t sp_w, uint32_t sp_h,
                       uint16_t colorkey);

    /* ── PPA Framebuffer Copy (hardware DMA copy via PPA SRM) ────────── */
    int (*fb_copy)(const uint16_t *src, uint16_t *dst,
                   uint32_t w, uint32_t h);

    /* ────────────────────────────────────────────────────────────────
     * APPEND-ONLY ZONE — added after ABI v1 shipped.
     *
     * New services are appended here, never inserted above, so existing
     * .papp binaries keep their field offsets and run unchanged without
     * a version bump. A new field may be NULL if an OLDER launcher loads
     * a newer app, so touch-aware apps MUST null-check before calling:
     *     if (svc->touch_read && svc->touch_read(&x, &y)) { ... }
     * Loaders since display_get_size/display_set_canvas leave
     * PAPP_SERVICES_SPARE_SLOTS zeroed pointers after the table, so a field
     * appended later reads NULL on them. Loaders built before those two
     * fields end the table at net_resolve and whatever follows it is NOT
     * zero: an app that calls a field added after net_resolve needs a
     * loader that has it.
     * ──────────────────────────────────────────────────────────────── */

    /* ── Touch (GT911) ───────────────────────────────────────────────── */
    /* Read the capacitive touch panel. Coordinates are reported in the
     * LANDSCAPE native-framebuffer space after the panel's 180-degree display
     * transform, inside the app's canvas: x in [0, width-1], y in
     * [0, height-1] — [0,799] x [0,479] unless the app chose another canvas
     * with display_set_canvas. Touches outside the canvas (and on the
     * loader's close control beside it) are not reported.
     * Returns 1 if currently touched (and fills *x,*y), 0 if not. Either
     * pointer may be NULL. */
    int (*touch_read)(int *x, int *y);

    /* ── Analog paddle / wheel (ADC2_CH2, GPIO 51) ───────────────────── */
    /* Raw 12-bit reading (0..4095) from the physical analog wheel — the
     * same potentiometer the Atari paddle games use. Returns -1 when it is
     * unavailable: on the HDMI build (no paddle is wired) or if the ADC
     * could not be initialised. The ADC is brought up lazily on first call.
     *
     * The reading is refreshed by the launcher inside input_gamepad_read(),
     * so poll that every frame to keep values fresh.
     *
     * Appended after touch_read — null-check before calling:
     *     if (svc->paddle_read) { int raw = svc->paddle_read(); ... } */
    int (*paddle_read)(void);

    /* ── Dedicated PAPP close control (L3) ───────────────────────────── */
    /* Returns 1 while the configured physical L3 button is held. This is
     * appended after paddle_read so existing PAPP binaries remain ABI-safe. */
    int (*input_l3_read)(void);

    /* ── USB mouse events ────────────────────────────────────────────── */
    /* Returns 1 when a mouse report has been accumulated since the last
     * call. dx/dy are relative motion and buttons uses HID bits 1/2/4 for
     * left/right/middle. Appended so existing ABI-v1 binaries remain safe. */
    int (*input_mouse_read)(int *dx, int *dy, int *buttons);

    /* ── USB keyboard text/events ────────────────────────────────────── */
    /* Pop one queued keyboard event. Returns 1 when an event was returned,
     * 0 when the queue is empty. Printable keys use ASCII values; special
     * keys use the Quake key constants in the app. */
    int (*input_keyboard_read)(papp_keyboard_event_t *event);

    /* ── UDP networking (lwIP) ───────────────────────────────────────── */
    /* Minimal UDP for LAN multiplayer. IPv4 addresses and ports are in HOST
     * byte order (0xC0A80A05 is 192.168.10.5).
     *   net_udp_open   Bind a non-blocking UDP socket to `port` on every
     *                  interface (0: any free port); SO_BROADCAST when
     *                  `broadcast` is non-zero. Returns a handle >= 0, or -1.
     *   net_udp_send   Send `len` bytes to ip:port. Returns the bytes sent,
     *                  0 when the stack would block, -1 on error.
     *   net_udp_recv   Take one waiting datagram without blocking. Returns its
     *                  length (at most `len`), 0 when none is waiting, -1 on
     *                  error; `ip`/`port` (may be NULL) receive the sender.
     *   net_udp_close  Close a handle. The loader closes any the app leaves
     *                  open when it exits.
     *   net_ipv4       The device's IPv4 address and netmask (either may be
     *                  NULL). Returns 1 when the network is up, else 0.
     * Appended after input_keyboard_read — null-check before calling. */
    int  (*net_udp_open)(uint16_t port, int broadcast);
    int  (*net_udp_send)(int handle, const void *buf, int len, uint32_t ip, uint16_t port);
    int  (*net_udp_recv)(int handle, void *buf, int len, uint32_t *ip, uint16_t *port);
    void (*net_udp_close)(int handle);
    int  (*net_ipv4)(uint32_t *ip, uint32_t *netmask);

    /* ── TCP networking (lwIP) ───────────────────────────────────────── */
    /* Same byte-order rules as UDP. TCP handles share the UDP handle table;
     * close them with net_udp_close. Nothing here blocks except net_resolve.
     *   net_tcp_connect  Start connecting to ip:port. Returns a handle >= 0
     *                    (net_poll reports it writable once connected, or
     *                    failed), or -1.
     *   net_tcp_listen   Accept connections on `port` on every interface.
     *                    Returns a handle >= 0, or -1.
     *   net_tcp_accept   Take one waiting connection from a listening handle.
     *                    Returns its handle, -2 when none is waiting, -1 on
     *                    error; `ip`/`port` (may be NULL) receive the peer.
     *   net_tcp_send     Send up to `len` bytes. Returns the bytes taken,
     *                    0 when the stack would block, -1 on a failed link.
     *   net_tcp_recv     Read up to `len` bytes. Returns the bytes read, 0 when
     *                    the peer closed, -2 when nothing is waiting, -1 on
     *                    error.
     *   net_poll         Readiness of any handle: bit 0 readable (data, a
     *                    waiting connection or a close), bit 1 writable (e.g.
     *                    a finished connect), bit 2 failed (e.g. refused).
     *                    Returns 0 when nothing is ready, -1 for a bad handle.
     *   net_resolve      IPv4 address of `host` (a name or a dotted quad),
     *                    in host byte order. May block while DNS answers.
     *                    Returns 1 and sets *ip, or 0.
     * Appended after net_ipv4 — null-check before calling. */
    int  (*net_tcp_connect)(uint32_t ip, uint16_t port);
    int  (*net_tcp_listen)(uint16_t port);
    int  (*net_tcp_accept)(int handle, uint32_t *ip, uint16_t *port);
    int  (*net_tcp_send)(int handle, const void *buf, int len);
    int  (*net_tcp_recv)(int handle, void *buf, int len);
    int  (*net_poll)(int handle);
    int  (*net_resolve)(const char *host, uint32_t *ip);

    /* ── Display canvas size ─────────────────────────────────────────── */
    /* The canvas is the part of the panel an app draws. Every app starts
     * with the ABI v1 canvas of 800x480, centred on the panel, so apps that
     * never call these two services behave exactly as before. The canvas
     * size applies to everything on the display side: display_get_framebuffer
     * (width x height RGB565, stride = width), display_flush, display_clear,
     * display_write_frame_rgb565 (one full canvas), display_write_rect
     * (clipped to the canvas), display_write_frame_custom / display_emu_flush
     * (the scaled frame is centred in the canvas and at most its size),
     * touch_read (canvas coordinates) and the loader's screenshots.
     *
     *   display_get_size    The canvas this app should use: the user's
     *                       per-app Screen setting from the store, else the
     *                       device's default canvas (YAML canvas_width /
     *                       canvas_height, normally the whole panel, e.g.
     *                       1024x600). Once the app has switched with
     *                       display_set_canvas it reports the canvas in use.
     *                       Either pointer may be NULL.
     *   display_set_canvas  Switch to a width x height canvas. Both must be
     *                       even, at least 320x240 and at most the panel
     *                       (display_get_size's answer always qualifies).
     *                       Returns 0 on success; -1 refuses the size and
     *                       leaves the canvas as it was. A change clears the
     *                       framebuffer and the whole panel to black; asking
     *                       for the current size changes nothing. Call it
     *                       before drawing, from the task that draws, and
     *                       read display_get_framebuffer afterwards. On a
     *                       canvas too wide for the close control beside it,
     *                       the control is not drawn: taps in the canvas's
     *                       top-right corner reach the app and only a 2 s
     *                       hold there closes it, so offer your own exit.
     *
     * Typical use:
     *     int w = 800, h = 480;
     *     if (svc->display_get_size && svc->display_set_canvas) {
     *         svc->display_get_size(&w, &h);
     *         if (svc->display_set_canvas(w, h) != 0) { w = 800; h = 480; }
     *     }
     *     uint16_t *fb = svc->display_get_framebuffer();   // w x h
     * An app with fixed sizes picks the largest of its own that fits in
     * display_get_size's answer and passes that to display_set_canvas.
     * Appended after net_resolve — null-check before calling (an older
     * launcher leaves them NULL and the canvas is always 800x480). */
    void (*display_get_size)(int *width, int *height);
    int  (*display_set_canvas)(int width, int height);

    /* ── MIDI (a USB-MIDI cable or keyboard, usb_midi) ─────────────────
     *   midi_read   Copies up to len received MIDI bytes into buf and
     *               returns how many (0: nothing new). Plain MIDI, whole
     *               messages, status byte first: 90 3C 64 is note on,
     *               middle C. Bytes that came before the app started are
     *               dropped.
     *   midi_write  Sends whole MIDI messages (status byte first; SysEx
     *               F0 ... F7 included; no running status). Returns 0 when
     *               queued, -1 when no device is plugged in, the loader has
     *               no usb_midi or the queue is full.
     * Appended after display_set_canvas: NULL on older loaders. */
    int  (*midi_read)(uint8_t *buf, int len);
    int  (*midi_write)(const uint8_t *data, int len);

    /* ── Multi-touch ────────────────────────────────────────────────────
     *   touch_read_points  Fills up to max points with the fingers on the
     *                      panel now (the first finger first) and returns
     *                      how many; 0: none. Same canvas coordinates as
     *                      touch_read; fingers outside the canvas are left
     *                      out. The GT911 reports up to 5.
     * Appended after midi_write: NULL on older loaders. */
    int  (*touch_read_points)(papp_touch_point_t *points, int max);

} app_services_t;

/* ── Entry Point Signature ───────────────────────────────────────────── */
/*
 * Every PSRAM app must export this function at the entry_off specified
 * in its .papp header. It receives the service table and returns 0 on
 * success or a negative error code.
 */
typedef int (*papp_entry_fn_t)(const app_services_t *svc);

/* ── Loader API (launcher-side only) ─────────────────────────────────── */
#ifndef PAPP_APP_SIDE  /* Excluded when building PSRAM apps */

#include "esp_err.h"

/* Opaque handle for a loaded PSRAM app */
typedef struct psram_app *psram_app_handle_t;

/**
 * Load a .papp binary from the filesystem into PSRAM.
 * Does NOT execute it yet.
 *
 * @param path   Absolute path, e.g. "/sd/roms/papp/opentyrian.papp"
 * @param handle Receives the loaded app handle
 * @return ESP_OK on success
 */
esp_err_t psram_app_load(const char *path, psram_app_handle_t *handle);

/**
 * Load a .papp binary over HTTP(S) into PSRAM without writing it to the SD
 * card. The binary is streamed into its final PSRAM allocation and is fully
 * validated before it can execute.
 *
 * @param url    HTTP(S) URL serving one complete .papp binary
 * @param handle Receives the loaded app handle
 * @return ESP_OK on success
 */
esp_err_t psram_app_load_url(const char *url, psram_app_handle_t *handle);

/**
 * Execute a previously loaded PSRAM app.
 * Maps code as executable, calls the entry point, waits for return,
 * then unmaps. Blocks until the app returns.
 *
 * @param handle  Loaded app handle
 * @return The app's return code (0 = success)
 */
int psram_app_run(psram_app_handle_t handle);

/**
 * Unload a PSRAM app and free all resources.
 *
 * @param handle  Loaded app handle (NULL-safe)
 */
void psram_app_unload(psram_app_handle_t handle);

/**
 * Self-test: verify PSRAM XIP works by loading and executing a tiny
 * built-in function from PSRAM. Logs results.
 *
 * @return ESP_OK if PSRAM code execution works
 */
esp_err_t psram_app_selftest(void);

#endif /* !PAPP_APP_SIDE */

#ifdef __cplusplus
}
#endif
