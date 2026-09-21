/* Atari 800/XL/5200 PAPP entry point. */
#define PAPP_APP_SIDE 1

#include "runtime.h"
#include "atari800_run.h"

#include <setjmp.h>

const app_services_t *_papp_svc;
static jmp_buf s_exit_env;

extern "C" void app_return_to_launcher(void)
{
    longjmp(s_exit_env, 1);
}

extern "C" __attribute__((section(".text.entry")))
int app_entry(const app_services_t *svc)
{
    if (!svc || svc->abi_version != PAPP_ABI_VERSION) return -1;
    _papp_svc = svc;

    char rom_path[512];
    if (emu_rom_path("atari800", ".atr|.xex|.com|.cas|.a52|.car|.rom|.bin|",
                     rom_path, sizeof(rom_path))) {
        svc->log_printf("Atari800: set /sd/roms/papp/atari800.rom to an Atari image\n");
        return -1;
    }

    int result = -1;
    if (setjmp(s_exit_env)) goto cleanup;

    svc->log_printf("Atari800 PAPP: loading %s\n", rom_path);
    atari800_run(rom_path);
    result = 0;

cleanup:
    papp_close_streams();
    papp_cleanup_fds();
    papp_cleanup_heap();
    if (svc->display_clear) svc->display_clear(0);
    if (svc->display_flush) svc->display_flush();
    return result;
}
