/* Game & Watch PAPP wrapper for the GPLv3 LCD-Game-Emulator core. */
#define PAPP_APP_SIDE 1

#include "runtime.h"
#include "gw_system.h"
#include "gw_romloader.h"
#include "rom_manager.h"

#include <stdint.h>
#include <stddef.h>
#include <setjmp.h>
#include <string.h>
#include <time.h>

const app_services_t *_papp_svc;

/* The upstream core expects these globals to be supplied by its host. */
unsigned char *ROM_DATA;
unsigned int ROM_DATA_LENGTH;

static uint16_t *s_frame;
static int16_t *s_audio;
static jmp_buf s_exit_env;

extern void app_return_to_launcher(void);

void app_return_to_launcher(void)
{
    longjmp(s_exit_env, 1);
}

unsigned int gw_get_buttons(void)
{
    papp_gamepad_state_t pad;
    memset(&pad, 0, sizeof(pad));
    if (_papp_svc && _papp_svc->input_gamepad_read)
        _papp_svc->input_gamepad_read(&pad);

    if (emu_quit(&pad))
        app_return_to_launcher();

    unsigned int buttons = 0;
    if (pad.values[PAPP_INPUT_LEFT])   buttons |= GW_BUTTON_LEFT;
    if (pad.values[PAPP_INPUT_UP])     buttons |= GW_BUTTON_UP;
    if (pad.values[PAPP_INPUT_RIGHT])  buttons |= GW_BUTTON_RIGHT;
    if (pad.values[PAPP_INPUT_DOWN])   buttons |= GW_BUTTON_DOWN;
    if (pad.values[PAPP_INPUT_A])      buttons |= GW_BUTTON_A;
    if (pad.values[PAPP_INPUT_B])      buttons |= GW_BUTTON_B;
    if (pad.values[PAPP_INPUT_SELECT]) buttons |= GW_BUTTON_TIME;
    if (pad.values[PAPP_INPUT_START])  buttons |= GW_BUTTON_GAME;
    return buttons;
}

/* The core emits a one-bit/short multi-level piezo stream at 32768 Hz.  Turn
 * it into the signed interleaved stereo format used by the PAPP audio queue. */
static void submit_audio(void)
{
    if (!_papp_svc || !_papp_svc->audio_submit || !s_audio) return;
    for (unsigned i = 0; i < GW_AUDIO_BUFFER_LENGTH; ++i) {
        int16_t sample = (int16_t)(((int)gw_audio_buffer[i] - 1) * 7000);
        s_audio[2u * i] = sample;
        s_audio[2u * i + 1u] = sample;
    }
    _papp_svc->audio_submit(s_audio, GW_AUDIO_BUFFER_LENGTH);
    gw_audio_buffer_copied = true;
}

static void set_emulated_time(void)
{
    time_t now = time(NULL);
    struct tm *local = localtime(&now);
    if (!local) return;
    gw_time_t t;
    t.hours = (unsigned char)local->tm_hour;
    t.minutes = (unsigned char)local->tm_min;
    t.seconds = (unsigned char)local->tm_sec;
    gw_system_set_time(t);
}

static void cleanup_core(void)
{
    if (GW_ROM) {
        _papp_svc->mem_free(GW_ROM);
        GW_ROM = NULL;
    }
    if (ROM_DATA) {
        _papp_svc->mem_free(ROM_DATA);
        ROM_DATA = NULL;
    }
}

__attribute__((section(".text.entry"), used))
int app_entry(const app_services_t *svc)
{
    if (!svc || svc->abi_version != PAPP_ABI_VERSION) return -1;
    _papp_svc = svc;
    s_frame = NULL;
    s_audio = NULL;
    ROM_DATA = NULL;
    ROM_DATA_LENGTH = 0;

    uint8_t *rom = NULL;
    size_t rom_size = 0;
    char rom_path[512];
    if (emu_load_rom("gw", ".gw|.lcd|", &rom, &rom_size,
                     rom_path, sizeof(rom_path))) {
        svc->log_printf("Game & Watch PAPP: set /sd/roms/papp/gw.rom to a LCD-Game-Shrinker .gw file\n");
        return -1;
    }
    if (rom_size > 400000u) {
        svc->log_printf("Game & Watch PAPP: ROM is too large (%u bytes)\n",
                        (unsigned)rom_size);
        svc->mem_free(rom);
        return -1;
    }

    s_frame = (uint16_t *)svc->mem_caps_alloc(
        GW_SCREEN_WIDTH * GW_SCREEN_HEIGHT * sizeof(uint16_t),
        PAPP_MEM_CAP_SPIRAM);
    s_audio = (int16_t *)svc->mem_caps_alloc(
        GW_AUDIO_BUFFER_LENGTH * 2u * sizeof(int16_t),
        PAPP_MEM_CAP_SPIRAM);
    if (!s_frame || !s_audio) goto cleanup;
    memset(s_frame, 0, GW_SCREEN_WIDTH * GW_SCREEN_HEIGHT * sizeof(uint16_t));
    memset(s_audio, 0, GW_AUDIO_BUFFER_LENGTH * 2u * sizeof(int16_t));

    ROM_DATA = rom;
    ROM_DATA_LENGTH = (unsigned int)rom_size;
    svc->audio_init(GW_AUDIO_FREQ);
    svc->log_printf("Game & Watch PAPP: loading %s (%u bytes)\n",
                    rom_path, (unsigned)rom_size);

    if (setjmp(s_exit_env)) goto cleanup_core_and_buffers;
    if (!gw_system_romload()) {
        svc->log_printf("Game & Watch PAPP: ROM load failed\n");
        goto cleanup_core_and_buffers;
    }
    gw_system_sound_init();
    if (!gw_system_config()) {
        svc->log_printf("Game & Watch PAPP: unsupported CPU in ROM\n");
        goto cleanup_core_and_buffers;
    }
    gw_system_start();
    gw_system_reset();
    set_emulated_time();
    svc->log_printf("Game & Watch PAPP: running %s\n", rom_path);

    {
        emu_clock clock = { svc->get_time_us(), 0, GW_REFRESH_RATE };
        unsigned frames = 0;
        for (;;) {
            gw_system_run(GW_SYSTEM_CYCLES);
            if ((frames++ & 1u) == 0u && svc->display_write_frame_custom) {
                gw_system_blit(s_frame);
                svc->display_write_frame_custom(s_frame, GW_SCREEN_WIDTH,
                                                GW_SCREEN_HEIGHT, 1.5f, false);
            }
            submit_audio();
            emu_pace(&clock);
            if ((frames & 127u) == 0u)
                svc->log_printf("Game & Watch PAPP: frames=%u\n", frames);
#ifdef PAPP_TEST_FRAMES
            if (frames >= PAPP_TEST_FRAMES) break;
#endif
        }
    }

cleanup_core_and_buffers:
    cleanup_core();
cleanup:
    if (s_audio) svc->mem_free(s_audio);
    if (s_frame) svc->mem_free(s_frame);
    s_audio = NULL;
    s_frame = NULL;
    return 0;
}
