/* WonderSwan / WonderSwan Color PAPP wrapper for Oswan. */
#define PAPP_APP_SIDE 1

#include "runtime.h"
extern "C" {
#include "WS.h"
#include "WSRender.h"
#include "WSInput.h"
#include "WSFileio.h"
#include "WSApu.h"
}

#include <stdint.h>
#include <stddef.h>
#include <setjmp.h>
#include <string.h>

const app_services_t *_papp_svc;
static int16_t *s_audio;
static uint16_t *s_frame;
static jmp_buf s_exit_env;

extern "C" void init_ModRM_tables(void);
extern "C" void WsAllocateBuffers(void);

void *operator new(size_t size) noexcept
{
    return _papp_svc ? _papp_svc->mem_alloc(size) : NULL;
}
void *operator new[](size_t size) noexcept
{
    return _papp_svc ? _papp_svc->mem_alloc(size) : NULL;
}
void operator delete(void *p) noexcept
{
    if (p && _papp_svc) _papp_svc->mem_free(p);
}
void operator delete[](void *p) noexcept
{
    if (p && _papp_svc) _papp_svc->mem_free(p);
}
void operator delete(void *p, size_t) noexcept
{
    if (p && _papp_svc) _papp_svc->mem_free(p);
}
void operator delete[](void *p, size_t) noexcept
{
    if (p && _papp_svc) _papp_svc->mem_free(p);
}

extern "C" void app_return_to_launcher(void)
{
    longjmp(s_exit_env, 1);
}

extern "C" unsigned long SDL_UXTimerRead(void)
{
    return _papp_svc ? (unsigned long)_papp_svc->get_time_us() : 0;
}

static void submit_audio(void)
{
    if (!s_audio || !_papp_svc || !_papp_svc->audio_submit) return;
    int available = apuBufLen();
    if (available <= 0) return;
    if (available > 512) available = 512;
    for (int i = 0; i < available; ++i) {
        s_audio[2 * i] = sndbuffer[0][rBuf];
        s_audio[2 * i + 1] = sndbuffer[1][rBuf];
        if (++rBuf >= SND_RNGSIZE) rBuf = 0;
    }
    _papp_svc->audio_submit(s_audio, available);
}

/* Oswan renders into a 240-pixel stride with the 224x144 LCD image at the
 * left.  Copy only the visible region into a compact PAPP frame. */
extern "C" void ws_graphics_paint(void)
{
    if (!FrameBuffer || !_papp_svc || !_papp_svc->display_write_frame_custom)
        return;
    if (!s_frame) {
        s_frame = (uint16_t *)_papp_svc->mem_caps_alloc(
            LCD_MAIN_W * LCD_MAIN_H * sizeof(uint16_t), PAPP_MEM_CAP_SPIRAM);
        if (!s_frame) return;
    }
    for (int y = 0; y < LCD_MAIN_H; ++y) {
        memcpy(s_frame + y * LCD_MAIN_W,
               FrameBuffer + y * SCREEN_WIDTH,
               LCD_MAIN_W * sizeof(uint16_t));
    }
    _papp_svc->display_write_frame_custom(s_frame, LCD_MAIN_W, LCD_MAIN_H,
                                          2.0f, false);
    submit_audio();
}

enum {
    WS_Y1 = 1 << 0, WS_Y2 = 1 << 1, WS_Y3 = 1 << 2, WS_Y4 = 1 << 3,
    WS_X1 = 1 << 4, WS_X2 = 1 << 5, WS_X3 = 1 << 6, WS_X4 = 1 << 7,
    WS_OPTION = 1 << 8, WS_START = 1 << 9, WS_A = 1 << 10, WS_B = 1 << 11
};

extern "C" int ws_input_poll(int mode)
{
    papp_gamepad_state_t pad;
    memset(&pad, 0, sizeof(pad));
    _papp_svc->input_gamepad_read(&pad);
    if (emu_quit(&pad)) app_return_to_launcher();

    int state = 0;
    const int up = pad.values[PAPP_INPUT_UP];
    const int right = pad.values[PAPP_INPUT_RIGHT];
    const int down = pad.values[PAPP_INPUT_DOWN];
    const int left = pad.values[PAPP_INPUT_LEFT];
    if (mode == 0) {
        if (up) state |= WS_X1;
        if (right) state |= WS_X2;
        if (down) state |= WS_X3;
        if (left) state |= WS_X4;
    } else {
        if (up) state |= WS_Y1;
        if (right) state |= WS_Y2;
        if (down) state |= WS_Y3;
        if (left) state |= WS_Y4;
    }
    if (pad.values[PAPP_INPUT_A]) state |= WS_A;
    if (pad.values[PAPP_INPUT_B]) state |= WS_B;
    if (pad.values[PAPP_INPUT_SELECT]) state |= WS_OPTION;
    if (pad.values[PAPP_INPUT_START]) state |= WS_START;
    return state;
}

extern "C" __attribute__((section(".text.entry"), used))
int app_entry(const app_services_t *svc)
{
    if (!svc || svc->abi_version != PAPP_ABI_VERSION) return -1;
    _papp_svc = svc;
    s_audio = NULL;
    s_frame = NULL;

    uint8_t *rom = NULL;
    size_t rom_size = 0;
    char rom_path[512];
    if (emu_load_rom("ws", ".ws|.wsc|", &rom, &rom_size,
                     rom_path, sizeof(rom_path))) {
        svc->log_printf("WonderSwan PAPP: set /sd/roms/papp/ws.rom to a .ws or .wsc ROM\n");
        return -1;
    }

    s_audio = (int16_t *)svc->mem_caps_alloc(512u * 2u * sizeof(int16_t),
                                              PAPP_MEM_CAP_SPIRAM);
    if (!s_audio) {
        svc->mem_free(rom);
        return -1;
    }
    memset(s_audio, 0, 512u * 2u * sizeof(int16_t));
    svc->audio_init(12000);

    if (setjmp(s_exit_env)) goto cleanup;

    svc->log_printf("WonderSwan PAPP: loading %s (%u bytes)\n",
                    rom_path, (unsigned)rom_size);
    init_ModRM_tables();
    WsAllocateBuffers();
    /* WsReset() writes palette state during cartridge setup.  The renderer
     * owns Palette, SprTMap, and FrameBuffer, so these must exist first. */
    AllocateBuffers();
    apuInit();
    if (WsCreateFromMemory(rom, rom_size) != 0) {
        svc->log_printf("WonderSwan PAPP: ROM mapping failed\n");
        goto cleanup_core;
    }

    {
        emu_clock clock = { svc->get_time_us(), 0, 75 };
        unsigned frames = 0;
        svc->log_printf("WonderSwan PAPP: running %s\n", rom_path);
        for (;;) {
            WsRun();
            emu_pace(&clock);
            if ((++frames & 127u) == 0) svc->log_printf("WonderSwan PAPP: frames=%u\n", frames);
#ifdef PAPP_TEST_FRAMES
            if (frames >= PAPP_TEST_FRAMES) break;
#endif
        }
    }

cleanup_core:
    WsRelease();
    FreeBuffers();
    apuEnd();
cleanup:
    if (s_audio) svc->mem_free(s_audio);
    if (s_frame) svc->mem_free(s_frame);
    if (rom) svc->mem_free(rom);
    s_audio = NULL;
    s_frame = NULL;
    return 0;
}
