/* Atari 2600 / Stella PAPP entry point. */
#define PAPP_APP_SIDE 1

#include "runtime.h"
#include "stella_run.h"

#include <stddef.h>
#include <setjmp.h>

const app_services_t *_papp_svc;
static jmp_buf s_exit_env;

extern "C" void app_return_to_launcher(void)
{
    longjmp(s_exit_env, 1);
}

void *operator new(size_t size) noexcept
{
    return _papp_svc ? _papp_svc->mem_alloc(size) : NULL;
}
void *operator new[](size_t size) noexcept
{
    return _papp_svc ? _papp_svc->mem_alloc(size) : NULL;
}
void operator delete(void *ptr) noexcept
{
    if (ptr && _papp_svc) _papp_svc->mem_free(ptr);
}
void operator delete[](void *ptr) noexcept
{
    if (ptr && _papp_svc) _papp_svc->mem_free(ptr);
}
void operator delete(void *ptr, size_t) noexcept
{
    if (ptr && _papp_svc) _papp_svc->mem_free(ptr);
}
void operator delete[](void *ptr, size_t) noexcept
{
    if (ptr && _papp_svc) _papp_svc->mem_free(ptr);
}

/* The PAPP header uses entry_off=0. Keep the C++ entry point at the first
 * byte of the flat image, just like the C PAPP entry points. */
extern "C" __attribute__((section(".text.entry"), used))
int app_entry(const app_services_t *svc)
{
    if (!svc || svc->abi_version != PAPP_ABI_VERSION) return -1;
    _papp_svc = svc;

    char rom_path[512];
    if (emu_rom_path("stella", ".a26|.bin|", rom_path, sizeof(rom_path))) {
        svc->log_printf("Stella: set /sd/roms/papp/stella.rom to an .a26 or .bin ROM\n");
        return -1;
    }

    if (setjmp(s_exit_env)) {
        papp_close_streams();
        papp_cleanup_fds();
        papp_cleanup_heap();
        return -1;
    }

    stella_run(rom_path);
    return 0;
}
