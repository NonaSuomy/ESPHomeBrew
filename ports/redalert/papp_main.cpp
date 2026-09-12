// Red Alert PAPP entry point.
//
// app_entry() runs on the loader's small worker task. It stores the service
// table, runs the C++ global constructors (Vanilla Conquer has many: the unit
// heaps, Map, Rules, the timers...), then starts the game on its own task with a
// large stack and waits. exit()/abort() and the on-screen close button end up
// in papp_quit(), which longjmps back to the game task's start.
#include "papp_port.h"

#include <setjmp.h>
#include <stdint.h>
#include <string.h>

extern "C" {
const app_services_t *papp_svc = nullptr;
}

int main(int argc, char **argv);  // redalert/startup.cpp

typedef void (*init_fn)(void);
extern "C" init_fn __init_array_start[];
extern "C" init_fn __init_array_end[];

static jmp_buf s_exit_jmp;
static volatile bool s_game_running = false;
static volatile bool s_game_done = false;
static volatile int s_exit_code = 0;

extern "C" long long papp_time_us(void)
{
    return papp_svc->get_time_us();
}

extern "C" void papp_quit(int code)
{
    s_exit_code = code;
    if (s_game_running) {
        longjmp(s_exit_jmp, 1);
    }
    // Before the game task started (e.g. a constructor failed): nothing to unwind.
    papp_svc->log_printf("RA: quit(%d) before the game started\n", code);
    for (;;) {
        papp_svc->delay_ms(1000);
    }
}

static void game_task(void *)
{
    static char arg0[] = "redalert";
    static char *argv[] = {arg0, nullptr};
    if (setjmp(s_exit_jmp) == 0) {
        s_game_running = true;
        // Global constructors run here, on the game's big stack: some build
        // INI parsers and heaps, too much for the loader's 16 KiB worker task.
        const unsigned ctors = (unsigned)(__init_array_end - __init_array_start);
        papp_svc->log_printf("RA: running %u global constructors\n", ctors);
        for (init_fn* fn = __init_array_start; fn < __init_array_end; ++fn) {
            (*fn)();
        }
        papp_svc->log_printf("RA: constructors done, entering main()\n");
        s_exit_code = main(1, argv);
    }
    s_game_running = false;
    papp_svc->log_printf("RA: game ended (%d)\n", s_exit_code);
    // The loader does not reclaim an app's files or heap: hand everything back.
    // The mixer task reads the game's sound buffers, so it stops first.
    papp_sound_shutdown();
    papp_close_all_files();
    papp_free_all_memory();
    s_game_done = true;
    // The loader deletes this task; returning from a FreeRTOS task aborts.
    for (;;) {
        papp_svc->delay_ms(1000);
    }
}

extern "C" __attribute__((section(".text.entry"), used)) int app_entry(const app_services_t *svc)
{
    papp_svc = svc;
    svc->log_printf("RA: Vanilla Conquer Red Alert PAPP starting\n");

    void *handle = nullptr;
    // Deep call chains (dialogs, pathfinding) and a few large locals: give the
    // game the kind of stack a desktop build has (the loader puts stacks this
    // big in PSRAM). Core 0 like the loader's own worker; core 1 runs ESPHome's
    // loop (network API, touch), which a busy priority-5 game would starve.
    if (svc->task_create(game_task, "redalert", 512 * 1024, nullptr, 5, &handle, 0) != 0) {
        svc->log_printf("RA: could not create the game task\n");
        return -1;
    }
    while (!s_game_done) {
        svc->delay_ms(50);
    }
    svc->delay_ms(50);
    if (handle != nullptr) {
        svc->task_delete(handle);
    }
    return s_exit_code;
}
