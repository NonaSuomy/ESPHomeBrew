#define PAPP_APP_SIDE 1
#include "psram_app.h"
#include "runtime.h"
#include "snes9x.h"
#include "memmap.h"
#include "apu.h"
#include "cpuexec.h"
#include "display.h"
#include "gfx.h"
#include "ppu.h"
#include "soundux.h"
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const app_services_t *_papp_svc;
bool overclock_cycles = false;
int one_c = 4, slow_one_c = 5, two_c = 6;
char *rom_filename = NULL;

static uint16_t *snes_fb;
static uint8_t *snes_subscreen;
static uint8_t *snes_zbuffer;
static uint8_t *snes_subzbuffer;
static int16_t *snes_audio;
static jmp_buf exit_env;
static unsigned snes_frame_count;

int64_t papp_esp_timer_get_time(void)
{
    return _papp_svc ? _papp_svc->get_time_us() : 0;
}

void app_return_to_launcher(void) { longjmp(exit_env, 1); }

uint32_t S9xReadJoypad(int32_t port)
{
    if (port != 0) return 0;
    papp_gamepad_state_t pad;
    _papp_svc->input_gamepad_read(&pad);
#ifdef PAPP_TEST_FRAMES
    memset(&pad, 0, sizeof(pad));
    pad.values[PAPP_INPUT_START] = snes_frame_count >= 60 && snes_frame_count < 65;
    pad.values[PAPP_INPUT_A] = snes_frame_count >= 180 && snes_frame_count < 185;
#endif
    uint32_t joy = 0;
    if (pad.values[PAPP_INPUT_UP])     joy |= SNES_UP_MASK;
    if (pad.values[PAPP_INPUT_DOWN])   joy |= SNES_DOWN_MASK;
    if (pad.values[PAPP_INPUT_LEFT])   joy |= SNES_LEFT_MASK;
    if (pad.values[PAPP_INPUT_RIGHT])  joy |= SNES_RIGHT_MASK;
    if (pad.values[PAPP_INPUT_A])      joy |= SNES_A_MASK;
    if (pad.values[PAPP_INPUT_B])      joy |= SNES_B_MASK;
    if (pad.values[PAPP_INPUT_X])      joy |= SNES_X_MASK;
    if (pad.values[PAPP_INPUT_Y])      joy |= SNES_Y_MASK;
    if (pad.values[PAPP_INPUT_L])      joy |= SNES_TL_MASK;
    if (pad.values[PAPP_INPUT_R])      joy |= SNES_TR_MASK;
    if (pad.values[PAPP_INPUT_SELECT]) joy |= SNES_SELECT_MASK;
    if (pad.values[PAPP_INPUT_START])  joy |= SNES_START_MASK;
    return joy;
}

bool S9xReadMousePosition(int32_t which, int32_t *x, int32_t *y, uint32_t *buttons)
{ (void)which; (void)x; (void)y; (void)buttons; return false; }
bool S9xReadSuperScopePosition(int32_t *x, int32_t *y, uint32_t *buttons)
{ (void)x; (void)y; (void)buttons; return false; }
bool JustifierOffscreen(void) { return true; }
void JustifierButtons(uint32_t *justifiers) { (void)justifiers; }
void S9xToggleSoundChannel(int32_t channel) { (void)channel; }

bool S9xInitDisplay(void)
{
    GFX.Pitch = SNES_WIDTH * 2;
    GFX.ZPitch = SNES_WIDTH;
    GFX.Screen = (uint8_t *)snes_fb;
    GFX.SubScreen = snes_subscreen;
    GFX.ZBuffer = snes_zbuffer;
    GFX.SubZBuffer = snes_subzbuffer;
    return GFX.Screen && GFX.SubScreen && GFX.ZBuffer && GFX.SubZBuffer;
}
void S9xDeinitDisplay(void) {}

static void snes_clear_core(void)
{
    if (GFX.ZBuffer) { free(GFX.ZBuffer); GFX.ZBuffer = NULL; }
    if (GFX.SubZBuffer) { free(GFX.SubZBuffer); GFX.SubZBuffer = NULL; }
    S9xDeinitGFX();
    S9xDeinitAPU();
    S9xDeinitMemory();
    free(snes_fb); snes_fb = NULL;
    free(snes_subscreen); snes_subscreen = NULL;
    snes_zbuffer = snes_subzbuffer = NULL;
    free(snes_audio); snes_audio = NULL;
}

static int snes_load_rom(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long size = ftell(f);
    if (size < 1024 || size > MAX_ROM_SIZE) { fclose(f); return -1; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }
    Memory.ROM = calloc(1, MAX_ROM_SIZE);
    if (!Memory.ROM || fread(Memory.ROM, 1, (size_t)size, f) != (size_t)size) {
        fclose(f); return -1;
    }
    fclose(f);
    if (((size_t)size & 0x7ffu) == 512u) {
        memmove(Memory.ROM, Memory.ROM + 512, (size_t)size - 512);
        size -= 512;
    }
    Memory.ROM_Size = (uint32_t)size;
    return LoadROM(NULL) ? 0 : -1;
}

static unsigned snes_hash(void)
{
    unsigned h = 2166136261u;
    for (int i = 0; i < SNES_WIDTH * SNES_HEIGHT; i += 17)
        h = (h ^ snes_fb[i]) * 16777619u;
    return h;
}

__attribute__((section(".text.entry")))
int app_entry(const app_services_t *svc)
{
    if (!svc || svc->abi_version != PAPP_ABI_VERSION) return -1;
    _papp_svc = svc;
    char rom_path[512];
    if (emu_rom_path("snes", ".smc|.sfc|", rom_path, sizeof(rom_path))) {
        svc->log_printf("SNES: set ROM path in /sd/roms/papp/snes.rom\n");
        return -1;
    }

    int result = -1;
    if (setjmp(exit_env)) goto cleanup;

    snes_fb = calloc(SNES_WIDTH * SNES_HEIGHT_EXTENDED, sizeof(uint16_t));
    snes_subscreen = calloc(SNES_WIDTH * SNES_HEIGHT_EXTENDED, sizeof(uint16_t));
    snes_zbuffer = calloc(512 * SNES_HEIGHT_EXTENDED, 1);
    snes_subzbuffer = calloc(512 * SNES_HEIGHT_EXTENDED, 1);
    snes_audio = calloc((32000 / 60) * 2, sizeof(int16_t));
    if (!snes_fb || !snes_subscreen || !snes_zbuffer || !snes_subzbuffer || !snes_audio)
        app_return_to_launcher();

    Settings.CyclesPercentage = 100;
    Settings.H_Max = SNES_CYCLES_PER_SCANLINE;
    Settings.FrameTimePAL = 20000;
    Settings.FrameTimeNTSC = 16667;
    Settings.ControllerOption = SNES_JOYPAD;
    Settings.HBlankStart = (256 * Settings.H_Max) / SNES_HCOUNTER_MAX;
    Settings.SoundPlaybackRate = 32000;
    Settings.DisableSoundEcho = false;
    Settings.InterpolatedSound = true;

    if (!S9xInitMemory() || !S9xInitAPU() || !S9xInitSound(0, 0) ||
        !S9xInitDisplay() || !S9xInitGFX() || snes_load_rom(rom_path) < 0)
        app_return_to_launcher();
    S9xSetPlaybackRate(Settings.SoundPlaybackRate);
    svc->audio_init(32000);
    svc->log_printf("SNES PAPP: running %s (%s)\n", rom_path,
                    Settings.PAL ? "PAL" : "NTSC");

    int64_t deadline = svc->get_time_us();
    unsigned frame_count = 0, changes = 0, last_hash = 0;
    snes_frame_count = 0;
    int peak = 0;
    for (;;) {
        papp_gamepad_state_t pad;
        svc->input_gamepad_read(&pad);
#ifdef PAPP_TEST_FRAMES
        memset(&pad, 0, sizeof(pad));
        pad.values[PAPP_INPUT_START] = frame_count >= 60 && frame_count < 65;
        pad.values[PAPP_INPUT_A] = frame_count >= 180 && frame_count < 185;
#endif
        if (pad.values[PAPP_INPUT_MENU] && !pad.values[PAPP_INPUT_X] && !pad.values[PAPP_INPUT_Y])
            break;
        IPPU.RenderThisFrame = true;
        S9xMainLoop();
        svc->display_write_frame_custom(snes_fb, SNES_WIDTH, SNES_HEIGHT, 2.0f, false);
        S9xMixSamples(snes_audio, (32000 / 60) * 2);
        svc->audio_submit(snes_audio, 32000 / 60);
#ifdef PAPP_TEST_FRAMES
        {
            unsigned hash = snes_hash();
            if (frame_count && hash != last_hash) ++changes;
            last_hash = hash;
            for (int i = 0; i < (32000 / 60) * 2; ++i) {
                int v = snes_audio[i]; if (v < 0) v = -v;
                if (v > peak) peak = v;
            }
        }
#endif
        ++frame_count;
        snes_frame_count = frame_count;
#ifdef PAPP_TEST_FRAMES
        if (frame_count >= PAPP_TEST_FRAMES) {
            svc->log_printf("SNES test: frames=%u changed=%u audio_peak=%d elapsed_ms=%ld\n",
                            frame_count, changes, peak,
                            (long)((svc->get_time_us() - deadline) / 1000));
            break;
        }
#endif
        int hz = Settings.PAL ? 50 : 60;
        deadline += 1000000 / hz;
        int64_t now = svc->get_time_us();
        if (deadline > now) svc->delay_ms((int)((deadline - now) / 1000));
        else if (now - deadline > 50000) deadline = now;
    }
    result = 0;

cleanup:
    snes_clear_core();
    papp_close_streams();
    papp_cleanup_fds();
    papp_cleanup_heap();
    svc->display_clear(0);
    svc->display_flush();
    return result;
}
