/* Extended NES PAPP wrapper.
 *
 * This uses the newer Retro-Go nofrendo core so FDS and NSF images can be
 * selected without changing the established River City Ransom NES PAPP. */
#define PAPP_APP_SIDE 1

#include "runtime.h"
#include "nofrendo.h"
#include "nes/nes.h"
#include "nes/input.h"

#include <stdint.h>
#include <stddef.h>
#include <setjmp.h>
#include <string.h>

const app_services_t *_papp_svc;
static nes_t *s_nes;
static uint8_t *s_frame8;
static uint16_t *s_frame565;
static uint16_t *s_palette;
static jmp_buf s_exit_env;
static char s_fds_bios[96];

void app_return_to_launcher(void)
{
    longjmp(s_exit_env, 1);
}

static void submit_frame(uint8_t *bmp)
{
    if (!_papp_svc || !_papp_svc->display_write_frame_custom ||
        !s_frame565 || !s_palette || !bmp) return;
    for (unsigned y = 0; y < NES_SCREEN_HEIGHT; ++y) {
        const uint8_t *src = bmp + y * NES_SCREEN_PITCH + NES_SCREEN_OVERDRAW;
        uint16_t *dst = s_frame565 + y * NES_SCREEN_WIDTH;
        for (unsigned x = 0; x < NES_SCREEN_WIDTH; ++x)
            dst[x] = s_palette[src[x]];
    }
    _papp_svc->display_write_frame_custom(s_frame565, NES_SCREEN_WIDTH,
                                           NES_SCREEN_HEIGHT, 2.4f, false);
}

static void blit_screen(uint8_t *bmp)
{
    submit_frame(bmp);
}

static void poll_input(void)
{
    papp_gamepad_state_t pad;
    memset(&pad, 0, sizeof(pad));
    if (_papp_svc && _papp_svc->input_gamepad_read)
        _papp_svc->input_gamepad_read(&pad);
    if (emu_quit(&pad)) app_return_to_launcher();

    int state = 0;
    if (pad.values[PAPP_INPUT_A])      state |= NES_PAD_A;
    if (pad.values[PAPP_INPUT_B])      state |= NES_PAD_B;
    if (pad.values[PAPP_INPUT_SELECT]) state |= NES_PAD_SELECT;
    if (pad.values[PAPP_INPUT_START])  state |= NES_PAD_START;
    if (pad.values[PAPP_INPUT_UP])     state |= NES_PAD_UP;
    if (pad.values[PAPP_INPUT_DOWN])   state |= NES_PAD_DOWN;
    if (pad.values[PAPP_INPUT_LEFT])   state |= NES_PAD_LEFT;
    if (pad.values[PAPP_INPUT_RIGHT])  state |= NES_PAD_RIGHT;
    input_update(0, state);
}

static const char *find_fds_bios(void)
{
    static const char *const paths[] = {
        "/sd/roms/papp/disksys.rom",
        "/sd/roms/nes/disksys.rom",
        "/sd/roms/system/disksys.rom"
    };
    if (!_papp_svc || !_papp_svc->file_open || !_papp_svc->file_close)
        return NULL;
    for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
        void *f = _papp_svc->file_open(paths[i], "rb");
        if (f) {
            _papp_svc->file_close(f);
            strncpy(s_fds_bios, paths[i], sizeof(s_fds_bios) - 1);
            s_fds_bios[sizeof(s_fds_bios) - 1] = '\0';
            return s_fds_bios;
        }
    }
    return NULL;
}

__attribute__((section(".text.entry"), used))
int app_entry(const app_services_t *svc)
{
    if (!svc || svc->abi_version != PAPP_ABI_VERSION) return -1;
    _papp_svc = svc;
    s_nes = NULL;
    s_frame8 = NULL;
    s_frame565 = NULL;
    s_palette = NULL;
    s_fds_bios[0] = '\0';

    uint8_t *rom_data = NULL;
    size_t rom_size = 0;
    char rom_path[512];
    if (emu_load_rom("nesplus", ".nes|.fds|.nsf|", &rom_data, &rom_size,
                     rom_path, sizeof(rom_path))) {
        svc->log_printf("NES+ PAPP: set /sd/roms/papp/nesplus.rom to a .nes, .fds, or .nsf image\n");
        return -1;
    }

    s_frame8 = (uint8_t *)svc->mem_caps_alloc(
        NES_SCREEN_PITCH * NES_SCREEN_HEIGHT, PAPP_MEM_CAP_SPIRAM);
    s_frame565 = (uint16_t *)svc->mem_caps_alloc(
        NES_SCREEN_WIDTH * NES_SCREEN_HEIGHT * sizeof(uint16_t),
        PAPP_MEM_CAP_SPIRAM);
    s_palette = (uint16_t *)svc->mem_caps_alloc(256u * sizeof(uint16_t),
                                                 PAPP_MEM_CAP_INTERNAL);
    if (!s_frame8 || !s_frame565 || !s_palette) goto cleanup;
    memset(s_frame8, 0, NES_SCREEN_PITCH * NES_SCREEN_HEIGHT);

    {
        uint16_t *palette = (uint16_t *)nofrendo_buildpalette(
            NES_PALETTE_PVM, 16);
        if (!palette) goto cleanup;
        memcpy(s_palette, palette, 256u * sizeof(uint16_t));
        svc->mem_free(palette);
    }

    const char *fds_bios = find_fds_bios();
    svc->log_printf("NES+ PAPP: loading %s (%u bytes)%s\n", rom_path,
                    (unsigned)rom_size,
                    fds_bios ? "" : " (FDS BIOS not found; only needed for .fds)");
    if (setjmp(s_exit_env)) goto cleanup_core;

    s_nes = nes_init(SYS_DETECT, 44100, true, fds_bios);
    if (!s_nes) {
        svc->log_printf("NES+ PAPP: core initialization failed\n");
        goto cleanup_core;
    }
    rom_t *cart = rom_loadmem(rom_data, rom_size);
    if (!cart) {
        svc->log_printf("NES+ PAPP: unsupported or invalid image\n");
        goto cleanup_core;
    }
    int result = nes_insertcart(cart);
    if (result < 0) {
        svc->log_printf("NES+ PAPP: cartridge insertion failed (%d)%s\n", result,
                        result == -3 ? "; supply disksys.rom for FDS" : "");
        /* nes_insertcart() shuts down the core on its failure path. */
        s_nes = NULL;
        goto cleanup_core;
    }
    nes_setvidbuf(s_frame8);
    s_nes->blit_func = blit_screen;
    svc->audio_init(44100);
    svc->log_printf("NES+ PAPP: running %s (%s)\n", rom_path,
                    s_nes->cart->type == ROM_TYPE_NSF ? "NSF" :
                    s_nes->cart->type == ROM_TYPE_FDS ? "FDS" : "NES");

    {
        emu_clock clock = { svc->get_time_us(), 0, (unsigned)s_nes->refresh_rate };
        unsigned frames = 0;
        for (;;) {
            poll_input();
            nes_emulate(true);
            if (s_nes->apu && s_nes->apu->buffer && s_nes->apu->samples_per_frame)
                svc->audio_submit(s_nes->apu->buffer,
                                  s_nes->apu->samples_per_frame);
            emu_pace(&clock);
            if ((++frames & 127u) == 0u)
                svc->log_printf("NES+ PAPP: frames=%u\n", frames);
#ifdef PAPP_TEST_FRAMES
            if (frames >= PAPP_TEST_FRAMES) break;
#endif
        }
    }

cleanup_core:
    if (s_nes) {
        nes_shutdown();
        s_nes = NULL;
    }
cleanup:
    if (s_palette) svc->mem_free(s_palette);
    if (s_frame565) svc->mem_free(s_frame565);
    if (s_frame8) svc->mem_free(s_frame8);
    if (rom_data) svc->mem_free(rom_data);
    s_palette = NULL;
    s_frame565 = NULL;
    s_frame8 = NULL;
    return 0;
}
