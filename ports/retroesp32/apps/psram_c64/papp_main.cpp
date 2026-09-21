/* Commodore 64 PAPP entry point.  The Frodo core is adapted from c64-go's
 * ODROID-GO port; all device I/O goes through the PAPP service table. */
#define PAPP_APP_SIDE 1

#include "runtime.h"
#include "psram_app.h"
#include "sysdeps.h"
#include "C64.h"
#include "Display.h"
#include "Prefs.h"

#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const app_services_t *_papp_svc;
ranctx pseudoRand;
char AppDirPath[1024];
static jmp_buf s_exit_env;

extern "C" int chdir(const char *path)
{
    (void)path;
    return -1;
}

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

static int load_blob(const char *const *paths, size_t path_count,
                     void *dst, size_t expected, const char **used_path)
{
    for (size_t i = 0; i < path_count; ++i) {
        FILE *file = fopen(paths[i], "rb");
        if (!file) continue;
        size_t got = fread(dst, 1, expected, file);
        fclose(file);
        if (got == expected) {
            if (used_path) *used_path = paths[i];
            return 0;
        }
    }
    return -1;
}

static int load_c64_roms(C64 *c64)
{
    static const char *basic[] = {
        "/sd/roms/c64/bios/Basic ROM",
        "/sd/roms/c64/bios/Basic ROM.rom",
        "/sd/roms/c64/bios/basic.rom"
    };
    static const char *kernal[] = {
        "/sd/roms/c64/bios/Kernal ROM",
        "/sd/roms/c64/bios/Kernal ROM.rom",
        "/sd/roms/c64/bios/kernal.rom"
    };
    static const char *character[] = {
        "/sd/roms/c64/bios/Char ROM",
        "/sd/roms/c64/bios/Char ROM.rom",
        "/sd/roms/c64/bios/char.rom"
    };
    static const char *drive[] = {
        "/sd/roms/c64/bios/1541 ROM",
        "/sd/roms/c64/bios/1541 ROM.rom",
        "/sd/roms/c64/bios/1541.rom"
    };
    const char *used = NULL;

    if (load_blob(basic, sizeof(basic) / sizeof(basic[0]),
                  c64->Basic, 0x2000, &used)) {
        _papp_svc->log_printf("C64 PAPP: missing 8 KiB BASIC ROM\n");
        return -1;
    }
    if (load_blob(kernal, sizeof(kernal) / sizeof(kernal[0]),
                  c64->Kernal, 0x2000, &used)) {
        _papp_svc->log_printf("C64 PAPP: missing 8 KiB KERNAL ROM\n");
        return -1;
    }
    if (load_blob(character, sizeof(character) / sizeof(character[0]),
                  c64->Char, 0x1000, &used)) {
        _papp_svc->log_printf("C64 PAPP: missing 4 KiB character ROM\n");
        return -1;
    }
    if (load_blob(drive, sizeof(drive) / sizeof(drive[0]),
                  c64->ROM1541, 0x4000, &used)) {
        _papp_svc->log_printf("C64 PAPP: missing 16 KiB 1541 ROM\n");
        return -1;
    }
    return 0;
}

extern "C" void c64_beforeStart(void) {}
extern "C" void c64_started(void) {}

/* The original ODROID-GO port used a writable host-side temp file while
 * extracting a PRG from a T64 image.  PAPP file handles are opaque, so create
 * the equivalent stream through the launcher-owned SD service. */
extern "C" FILE *_tmpfile(void)
{
    static unsigned sequence;
    if (!_papp_svc || !_papp_svc->file_open) return nullptr;
    char path[96];
    snprintf(path, sizeof(path), "/sd/roms/c64/.papp_tmp_%u", sequence++);
    return static_cast<FILE *>(_papp_svc->file_open(path, "w+b"));
}

extern "C" __attribute__((section(".text.entry"), used))
int app_entry(const app_services_t *svc)
{
    if (!svc || svc->abi_version != PAPP_ABI_VERSION ||
        !svc->file_open || !svc->audio_init ||
        !svc->display_write_frame_custom) return -1;
    _papp_svc = svc;
    if (setjmp(s_exit_env)) {
        if (svc->display_clear) svc->display_clear(0);
        if (svc->display_flush) svc->display_flush();
        return -1;
    }

    char rom_path[512];
    if (emu_rom_path("c64", ".d64|.t64|", rom_path, sizeof(rom_path))) {
        svc->log_printf("C64 PAPP: select a .d64 or .t64 image in the C64 ROM library\n");
        return -1;
    }

    const char *ext = strrchr(rom_path, '.');
    bool is_t64 = ext && (ext[1] == 't' || ext[1] == 'T');
    ThePrefs = Prefs();
    ThePrefs.DriveType[0] = is_t64 ? DRVTYPE_T64 : DRVTYPE_D64;
    strncpy(ThePrefs.DrivePath[0], rom_path,
            sizeof(ThePrefs.DrivePath[0]) - 1);
    ThePrefs.DrivePath[0][sizeof(ThePrefs.DrivePath[0]) - 1] = 0;
    for (int i = 1; i < 4; ++i) {
        ThePrefs.DriveType[i] = DRVTYPE_D64;
        ThePrefs.DrivePath[i][0] = 0;
    }
    ThePrefs.Emul1541Proc = false;
    ThePrefs.Joystick1On = false;
    ThePrefs.Joystick2On = true;
    ThePrefs.JoystickSwap = false;
    /* Use the PAPP digital SID renderer.  It submits stereo PCM through the
     * launcher audio service, which is already initialized below. */
    ThePrefs.SIDType = SIDTYPE_DIGITAL;
    ThePrefs.LimitSpeed = true;

    svc->log_printf("=== C64 PAPP: %s ===\n", rom_path);
    svc->log_printf("C64 PAPP: constructing Frodo\n");
    C64 *c64 = new C64();
    svc->log_printf("C64 PAPP: Frodo constructed\n");
    if (!c64) return -1;

    int result = -1;
    if (!load_c64_roms(c64)) {
        c64->TheDisplay->setAutoRun(!is_t64);
        svc->log_printf("C64 PAPP: running Frodo with %s\n",
                        is_t64 ? "T64" : "D64");
        c64->Run();
        result = 0;
    }

    delete c64;
    svc->display_clear(0);
    svc->display_flush();
    svc->log_printf("=== C64 PAPP exited (%d) ===\n", result);
    return result;
}
