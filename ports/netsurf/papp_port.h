// NetSurf on the ESP32-P4 PAPP loader: declarations shared by the port's glue
// code (ports/netsurf/*.c). Nothing here is part of NetSurf.
//
// Tasks (created through the loader's task_create service):
//   netsurf  NetSurf's framebuffer frontend: its scheduler, fetchers, layout
//            and drawing. Core 0, 512 KiB stack in PSRAM.
//   ns_dns   Host name lookups for the HTTP fetcher (net_resolve blocks).
#pragma once

#ifndef PAPP_APP_SIDE
#define PAPP_APP_SIDE 1
#endif

#include "esphome/components/papp_loader/psram_app.h"

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

// The loader's service table, set by app_entry before anything else runs.
extern const app_services_t *papp_svc;

// NetSurf's folder on the card: Choices, Cookies, Messages, and any resource
// (default.css, welcome.html, ...) that should replace the built-in copy.
#define PAPP_NS_DIR "/sd/roms/netsurf"

// Leave NetSurf and return to the loader. papp_request_quit() asks the
// frontend to close (the surface turns it into NSFB_CONTROL_QUIT);
// papp_quit() is for exit()/abort() and never returns.
void papp_request_quit(int code);
int papp_quit_requested(void);
void papp_quit(int code) __attribute__((noreturn));

// Microseconds since boot.
int64_t papp_time_us(void);

// Sleep one scheduler tick if the calling task has not slept for a while, so
// the idle task (and its watchdog) on this core still runs. papp_sleep_ms()
// sleeps and records that it did.
void papp_yield_if_due(void);
void papp_sleep_ms(int ms);

// Wall clock: time() is the loader's uptime until the first HTTP response
// brings a Date header; then it is that date plus the time since.
void papp_set_wallclock(time_t now);

// PSRAM (or internal RAM) straight from the loader, not in the app heap's
// list; freed with papp_free_raw.
void *papp_alloc_raw(size_t size, int internal);
void papp_free_raw(void *ptr);

// Heap, files and locks the app still holds, released when it quits.
void papp_syscalls_init(void);
void papp_syscalls_deinit(void);
void papp_free_all_memory(void);
void papp_close_all_files(void);

// Loader files (FILE * behind the service table), remembered so they can be
// closed when NetSurf quits.
void *papp_file_open(const char *path, const char *mode);
int papp_file_close(void *fp);
long papp_file_size(void *fp);
int papp_file_exists(const char *path);

// HTTP fetcher (papp_http.c): stops the host-name lookup task.
void papp_http_shutdown(void);

#ifdef __cplusplus
}
#endif
