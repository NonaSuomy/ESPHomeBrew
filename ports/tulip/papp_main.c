// Tulip PAPP entry point and the MicroPython task.
//
// app_entry() runs on the loader's worker task. It starts the MicroPython
// task and waits until Tulip is asked to quit (Menu held 3 s, the loader's
// close control, _papp.quit(), Ctrl-D at the REPL, or a fatal error), then
// stops the tasks and hands every file and heap block back to the loader.
//
// The MicroPython task does what Tulip Desktop's main() and main_() do
// (tulip/linux/main.c): start the display and AMY, init MicroPython with its
// heap in PSRAM, set up LVGL, run _boot.py and boot.py, then the REPL.
#include "papp_port.h"

#include <stdio.h>
#include <string.h>

#include "py/compile.h"
#include "py/cstack.h"
#include "py/gc.h"
#include "py/mperrno.h"
#include "py/mphal.h"
#include "py/runtime.h"
#include "shared/readline/readline.h"
#include "shared/runtime/gchelper.h"
#include "shared/runtime/pyexec.h"

const app_services_t *papp_svc = NULL;
volatile int papp_mp_ready = 0;

static volatile int s_quit = 0;
static volatile int s_quit_code = 0;
static volatile int s_mp_parked = 0;
static void *s_mp_task = NULL;
static char *s_heap = NULL;
static size_t s_heap_size = 0;

// The MicroPython task's stack. Bigger than a loader worker's 32 KiB, so the
// loader puts it in PSRAM.
#define MP_TASK_STACK_BYTES (96 * 1024)

volatile int *papp_quit_flag(void)
{
    return &s_quit;
}

void papp_request_quit(int code)
{
    if (!s_quit) {
        s_quit_code = code;
        s_quit = 1;
    }
}

// Any of the app's tasks: ask app_entry to shut Tulip down, then wait here
// to be deleted (a FreeRTOS task must not return).
void papp_quit(int code)
{
    papp_request_quit(code);
    for (;;) {
        papp_svc->delay_ms(1000);
    }
}

static int64_t s_last_sleep_us = 0;

// The VM hook and every wait go through here: park the MicroPython task
// for app_entry when Tulip quits, and sleep one real FreeRTOS tick now and
// then so the idle task (and its watchdog) on this core still runs.
void papp_mp_poll(void)
{
    if (s_quit) {
        s_mp_parked = 1;
        for (;;) {
            papp_svc->delay_ms(1000);
        }
    }
    const int64_t now = papp_time_us();
    if (now - s_last_sleep_us > 200000) {
        papp_svc->delay_ms(10);
        s_last_sleep_us = papp_time_us();
    }
}

void papp_mp_wait(void)
{
    papp_mp_poll();
    papp_svc->delay_ms(10);  // one tick (100 Hz); shorter delays are only a yield
    s_last_sleep_us = papp_time_us();
}

// ── GC ──────────────────────────────────────────────────────────────────────

// The app's .data and .bss (papp_cpp.ld) are scanned too: Tulip's C code
// keeps Python callbacks in plain globals (frame, touch, keyboard, sequencer).
extern char _data_start[];
extern char _bss_end[];

void gc_collect(void)
{
    gc_collect_start();
    gc_collect_root((void **)_data_start, ((uintptr_t)_bss_end - (uintptr_t)_data_start) / sizeof(void *));
    gc_helper_collect_regs_and_stack();
    gc_collect_end();
}

void nlr_jump_fail(void *val)
{
    papp_svc->log_printf("TULIP: FATAL uncaught NLR %p\n", val);
    papp_quit(-3);
}

void __assert_func(const char *file, int line, const char *func, const char *expr)
{
    papp_svc->log_printf("TULIP: assert '%s' failed at %s:%d (%s)\n", expr, file, line, func ? func : "");
    papp_quit(-4);
}

// ── Heap ────────────────────────────────────────────────────────────────────
// As big a GC heap as PSRAM allows, from 16 MiB down: MicroPython objects,
// LVGL (LV_STDLIB_MICROPYTHON allocates from it) and Tulip's Python code.
// C code keeps allocating after this (AMY samples and patches, PNG decoding,
// sprites), so a size is only taken if RESERVE bytes are still free next to it.

#define HEAP_RESERVE (4u << 20)

static void alloc_heap(void)
{
    static const size_t sizes[] = {16u << 20, 12u << 20, 8u << 20, 6u << 20, 4u << 20, 2u << 20};
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        s_heap = (char *)papp_alloc_raw(sizes[i], 0);
        if (s_heap == NULL) {
            continue;
        }
        void *probe = papp_alloc_raw(HEAP_RESERVE, 0);
        const int last = i + 1 == sizeof(sizes) / sizeof(sizes[0]);
        if (probe != NULL || last) {
            papp_free_raw(probe);
            s_heap_size = sizes[i];
            return;
        }
        papp_free_raw(s_heap);
        s_heap = NULL;
    }
}

// ── The MicroPython task ────────────────────────────────────────────────────

extern void setup_lvgl(void);
extern void tsequencer_init(void);

static void mp_task(void *arg)
{
    (void)arg;
    volatile int stack_dummy;
    s_last_sleep_us = papp_time_us();

    if (papp_display_start() != 0) {
        papp_quit(-10);
    }
    run_amy();
    tsequencer_init();

    alloc_heap();
    if (s_heap == NULL) {
        papp_svc->log_printf("TULIP: no memory for the MicroPython heap\n");
        papp_quit(-11);
    }
    papp_svc->log_printf("TULIP: MicroPython heap %u KiB in PSRAM\n", (unsigned)(s_heap_size >> 10));

    mp_cstack_init_with_top((void *)&stack_dummy, MP_TASK_STACK_BYTES - 1024);
    gc_init(s_heap, s_heap + s_heap_size);
    mp_init();
    readline_init0();
    setup_lvgl();
    papp_mp_ready = 1;

    pyexec_frozen_module("_boot.py", false);
    int ret = pyexec_file_if_exists("boot.py");
    if (!(ret & PYEXEC_FORCED_EXIT) && pyexec_mode_kind == PYEXEC_MODE_FRIENDLY_REPL) {
        ret = pyexec_file_if_exists("main.py");
    }
    if (!(ret & PYEXEC_FORCED_EXIT)) {
        for (;;) {
            if (pyexec_mode_kind == PYEXEC_MODE_RAW_REPL) {
                if (pyexec_raw_repl() != 0) {
                    break;
                }
            } else {
                if (pyexec_friendly_repl() != 0) {
                    break;
                }
            }
        }
    }
    // Ctrl-D on an empty line (or sys.exit from main.py). On a Tulip that is a
    // soft reboot (esp_restart); here it returns to the loader, which can
    // start Tulip again.
    mp_hal_stdout_tx_str("MPY: soft reboot, returning to the launcher\r\n");
    papp_request_quit(0);
    papp_mp_poll();  // parks here until app_entry deletes this task
}

// ── Entry ───────────────────────────────────────────────────────────────────

__attribute__((section(".text.entry"), used)) int app_entry(const app_services_t *svc)
{
    papp_svc = svc;
    papp_syscalls_init();
    // Set up newlib's stdio here, on one task, before the three tasks share
    // it (its locks are no-ops); unbuffered, so each printf reaches the log.
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    svc->log_printf("TULIP: Tulip Creative Computer PAPP starting\n");

    if (svc->task_create(mp_task, "tulip_mp", MP_TASK_STACK_BYTES, NULL, 5, &s_mp_task, 0) != 0) {
        svc->log_printf("TULIP: could not create the MicroPython task\n");
        return -1;
    }
    while (!s_quit) {
        svc->delay_ms(50);
    }
    svc->log_printf("TULIP: quitting (%d)\n", s_quit_code);

    papp_display_stop();
    papp_audio_stop();
    // Give the MicroPython task a moment to reach a safe point (VM hook,
    // input wait or sleep); a long C call can hold it up, so not forever.
    for (int i = 0; i < 60 && !s_mp_parked; i++) {
        svc->delay_ms(50);
    }
    if (!s_mp_parked) {
        svc->log_printf("TULIP: MicroPython did not stop in time, deleting it anyway\n");
    }
    if (s_mp_task != NULL) {
        svc->task_delete(s_mp_task);
        s_mp_task = NULL;
    }
    papp_close_all_files();
    papp_free_raw(s_heap);
    s_heap = NULL;
    papp_free_all_memory();
    svc->log_printf("TULIP: bye\n");
    return s_quit_code;
}
