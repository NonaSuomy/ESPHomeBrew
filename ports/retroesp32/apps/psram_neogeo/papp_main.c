/* Neo Geo GnGeo PAPP entry point. */
#define PAPP_APP_SIDE 1

#include "runtime.h"
#include "emu.h"
#include "roms.h"
#include "screen.h"
#include "sound.h"
#include "gnutil.h"

#include <setjmp.h>
#include <stdio.h>
#include <string.h>

const app_services_t *_papp_svc;

extern void esp32_set_rompath(const char *path);
extern void esp32_enable_sound(int enable);
extern void esp32_init_conf(const char *game_name);

static jmp_buf neo_exit_env;

void app_return_to_launcher(void)
{
    longjmp(neo_exit_env, 1);
}

static int neo_split_rom_path(const char *path, char *dir, size_t dir_size,
                              char *game, size_t game_size)
{
    const char *slash = strrchr(path, '/');
    const char *base = slash ? slash + 1 : path;
    const char *dot = strrchr(base, '.');
    if (!slash || !base[0] || !dot || dot == base) return -1;
    size_t dlen = (size_t)(slash - path);
    size_t glen = (size_t)(dot - base);
    if (!dlen || dlen >= dir_size || !glen || glen >= game_size) return -1;
    memcpy(dir, path, dlen);
    dir[dlen] = 0;
    memcpy(game, base, glen);
    game[glen] = 0;
    /* GnGeo's driver archive uses lowercase short names. */
    for (size_t i = 0; i < glen; ++i)
        if (game[i] >= 'A' && game[i] <= 'Z') game[i] += 'a' - 'A';
    return 0;
}

__attribute__((section(".text.entry")))
int app_entry(const app_services_t *svc)
{
    if (!svc || svc->abi_version != PAPP_ABI_VERSION) return -1;
    _papp_svc = svc;

    char rom_path[512];
    char rom_dir[512];
    char game_name[128];
    if (emu_rom_path("neogeo", ".zip|", rom_path, sizeof(rom_path)) ||
        neo_split_rom_path(rom_path, rom_dir, sizeof(rom_dir),
                           game_name, sizeof(game_name))) {
        svc->log_printf("NEO GEO: set /sd/roms/papp/neogeo.rom to a game ZIP\n");
        return -1;
    }

    if (setjmp(neo_exit_env)) goto cleanup;

    esp32_init_conf(game_name);
    esp32_set_rompath(rom_dir);
    esp32_enable_sound(1);
    svc->log_printf("NEO GEO PAPP: loading %s (%s)\n", game_name, rom_path);

    if (screen_init() != 0 || init_game(game_name) != GN_TRUE) {
        svc->log_printf("NEO GEO PAPP: failed to load %s: %s\n",
                        game_name, gnerror[0] ? gnerror : "no core error reported");
        goto cleanup;
    }

    neogeo_main_loop();

cleanup:
    close_game();
    close_sdl_audio();
    screen_close();
    papp_close_streams();
    papp_cleanup_fds();
    papp_cleanup_heap();
    return 0;
}
