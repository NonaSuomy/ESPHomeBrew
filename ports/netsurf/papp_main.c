// NetSurf PAPP entry point and the NetSurf task.
//
// app_entry() runs on the loader's worker task (16 KiB stack). It starts the
// NetSurf task with a big PSRAM stack and waits for NetSurf to finish: the
// frontend's close button, Menu/Escape held 3 s, Menu + X or the loader's
// close control (the surface turns those into NSFB_CONTROL_QUIT), or a fatal
// error. Then it stops the tasks and hands every file and heap block back to
// the loader, which does not reclaim them.
//
// The NetSurf task runs the global constructors (libnsfb registers its
// surfaces from them), switches to the canvas the loader offers (the whole
// panel, else 800x480), loads the built-in Messages and calls the
// framebuffer frontend's main() with the papp surface at that size, 16 bpp.
#include "papp_port.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "utils/errors.h"
#include "utils/messages.h"
#include "utils/nsoption.h"

#include "papp_resources.h"

const app_services_t *papp_svc = NULL;
int papp_canvas_w = 800;
int papp_canvas_h = 480;

static volatile int s_quit = 0;
static volatile int s_quit_code = 0;
static volatile int s_done = 0;
static int64_t s_quit_at = 0;
static void *s_task = NULL;

// NetSurf's stack: layout and the CSS engine recurse deeply on nested pages.
#define NETSURF_STACK_BYTES (512 * 1024)
// How long NetSurf gets to shut down cleanly (save cookies, free) once asked.
#define QUIT_GRACE_US 8000000

void papp_request_quit(int code)
{
    if (!s_quit) {
        s_quit_code = code;
        s_quit_at = papp_time_us();
        s_quit = 1;
        papp_svc->log_printf("NETSURF: quit requested (%d)\n", code);
    }
}

int papp_quit_requested(void)
{
    return s_quit;
}

// exit(), abort() or a failed assert on the NetSurf task: stop here and let
// app_entry clean up (a FreeRTOS task must not return).
void papp_quit(int code)
{
    papp_request_quit(code);
    s_done = 1;
    for (;;) {
        papp_svc->delay_ms(1000);
    }
}

// ── Hooks the patched frontend calls ────────────────────────────────────────

// frontends/framebuffer/gui.c set_defaults() (patches/0003): option defaults
// for this device. A Choices file in PAPP_NS_DIR still overrides them.
void papp_netsurf_set_defaults(void)
{
    nsoption_set_charp(cookie_file, strdup(PAPP_NS_DIR "/Cookies"));
    nsoption_set_charp(cookie_jar, strdup(PAPP_NS_DIR "/Cookies"));
    nsoption_set_int(window_width, papp_canvas_w);
    nsoption_set_int(window_height, papp_canvas_h);
    nsoption_set_bool(fb_osk, true);          // on-screen keyboard button, bottom right
    nsoption_set_int(fb_toolbar_size, 32);
    nsoption_set_int(fb_furniture_size, 22);  // scroll bars wide enough for a finger
    nsoption_set_int(memory_cache_size, 8 * 1024 * 1024);
    nsoption_set_int(max_fetchers, 4);        // the loader has 16 sockets for all apps' use
    nsoption_set_int(max_fetchers_per_host, 2);
    nsoption_set_int(max_cached_fetch_handles, 0);
    nsoption_set_bool(enable_javascript, false);
}

// frontends/framebuffer/fetch.c (patches/0004): resource: data compiled in.
// A copy in PAPP_NS_DIR wins; then NetSurf falls back to its file: URL.
nserror papp_get_resource_data(const char *path, const uint8_t **data, size_t *data_len)
{
    char on_card[256];
    snprintf(on_card, sizeof(on_card), "%s/%s", PAPP_NS_DIR, path);
    if (papp_file_exists(on_card)) {
        return NSERROR_NOT_FOUND;
    }
    for (const struct papp_resource *r = papp_resources; r->name != NULL; r++) {
        if (strcmp(r->name, path) == 0) {
            *data = r->data;
            *data_len = r->len;
            return NSERROR_OK;
        }
    }
    return NSERROR_NOT_FOUND;
}

nserror papp_release_resource_data(const uint8_t *data)
{
    (void)data;  // static data
    return NSERROR_OK;
}

// ── The NetSurf task ────────────────────────────────────────────────────────

typedef void (*init_fn)(void);
extern init_fn __init_array_start[];
extern init_fn __init_array_end[];

extern int main(int argc, char **argv);

static void netsurf_task(void *arg)
{
    (void)arg;
    for (init_fn *fn = __init_array_start; fn < __init_array_end; ++fn) {
        (*fn)();
    }

    // Built-in English Messages; a Messages file in PAPP_NS_DIR adds to them.
    if (messages_add_from_inline(papp_messages, papp_messages_len) != NSERROR_OK) {
        papp_svc->log_printf("NETSURF: built-in Messages failed to load\n");
    }

    // The canvas: what the loader offers (the store's Screen setting, else
    // the whole panel), from this task, which draws; 800x480 if refused.
    int w = 800, h = 480;
    if (papp_svc->display_get_size != NULL && papp_svc->display_set_canvas != NULL) {
        papp_svc->display_get_size(&w, &h);
        if (w < 320 || h < 240 || papp_svc->display_set_canvas(w, h) != 0) {
            w = 800;
            h = 480;
        }
    }
    papp_canvas_w = w;
    papp_canvas_h = h;
    papp_svc->log_printf("NETSURF: canvas %dx%d\n", w, h);

    static char width_arg[8], height_arg[8];
    snprintf(width_arg, sizeof(width_arg), "%d", w);
    snprintf(height_arg, sizeof(height_arg), "%d", h);
    static char *argv[16];
    int argc = 0;
    argv[argc++] = (char *)"netsurf";
    argv[argc++] = (char *)"-f";
    argv[argc++] = (char *)"papp";
    argv[argc++] = (char *)"-b";
    argv[argc++] = (char *)"16";
    argv[argc++] = (char *)"-w";
    argv[argc++] = width_arg;
    argv[argc++] = (char *)"-h";
    argv[argc++] = height_arg;
    // NetSurf's own log (verbose) when the card has PAPP_NS_DIR/verbose or
    // verbose.txt (tools that write files over the network need an extension).
    if (papp_file_exists(PAPP_NS_DIR "/verbose") || papp_file_exists(PAPP_NS_DIR "/verbose.txt")) {
        papp_svc->log_printf("NETSURF: verbose log on\n");
        argv[argc++] = (char *)"-v";
    }
    // The page to open: what the app that opened NetSurf passed, or the page
    // it asked to come back to after the video player (psram_app.h app_open).
    static char start_url[PAPP_APP_ARG_MAX];
    const int start_len = papp_svc->app_get_arg != NULL ? papp_svc->app_get_arg(start_url, sizeof(start_url)) : 0;
    if (start_len > 0 && start_len < (int)sizeof(start_url)) {
        papp_svc->log_printf("NETSURF: opening the page it was started with
");
        argv[argc++] = start_url;
    }
    argv[argc] = NULL;

    papp_svc->log_printf("NETSURF: starting the framebuffer frontend\n");
    const int rc = main(argc, argv);
    papp_svc->log_printf("NETSURF: frontend finished (%d)\n", rc);
    if (!s_quit) {
        s_quit_code = rc;
    }
    s_done = 1;
    for (;;) {
        papp_svc->delay_ms(1000);  // deleted by app_entry
    }
}

// ── Entry ───────────────────────────────────────────────────────────────────

__attribute__((section(".text.entry"), used)) int app_entry(const app_services_t *svc)
{
    papp_svc = svc;
    papp_syscalls_init();
    // Unbuffered, so every log line reaches the loader's log at once.
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    svc->log_printf("NETSURF: NetSurf PAPP starting (ABI %u)\n", (unsigned)svc->abi_version);
    if (svc->net_tcp_connect == NULL || svc->net_resolve == NULL) {
        svc->log_printf("NETSURF: this loader has no TCP services; only file:, about: and resource: pages work\n");
    }

    if (svc->task_create(netsurf_task, "netsurf", NETSURF_STACK_BYTES, NULL, 4, &s_task, 0) != 0) {
        svc->log_printf("NETSURF: could not create the NetSurf task\n");
        papp_syscalls_deinit();
        return -1;
    }
    while (!s_done) {
        svc->delay_ms(50);
        if (s_quit && papp_time_us() - s_quit_at > QUIT_GRACE_US) {
            svc->log_printf("NETSURF: NetSurf did not stop in time, stopping it anyway\n");
            break;
        }
    }
    svc->log_printf("NETSURF: quitting (%d)\n", s_quit_code);

    // NetSurf first, so no other task is inside a net_tls_* call on the
    // handles papp_http_shutdown closes.
    if (s_task != NULL) {
        svc->task_delete(s_task);
        s_task = NULL;
    }
    papp_http_shutdown();
    papp_close_all_files();
    papp_free_all_memory();
    papp_syscalls_deinit();
    svc->log_printf("NETSURF: bye\n");
    return s_quit_code;
}
