/* Atari Lynx (Handy) PAPP entry point.
 *
 * Handy's native ESP32 wrapper uses a video task to decode Mikie's compact
 * per-line framebuffer. A PAPP has no private task lifetime to manage, so it
 * drives CSystem on the app thread and decodes/submits each frame there.
 */
#define PAPP_APP_SIDE 1

#include "runtime.h"
#include "system.h"
#include "lynxdef.h"

#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const app_services_t *_papp_svc;

/* The loader already owns allocation and lifetime. Handy only needs the
 * basic C++ allocation ABI; linking libstdc++ would bring in thread-local
 * exception machinery that has no place in a freestanding PAPP. */
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

static jmp_buf exit_env;
static CSystem *lynx_system;
static uint8_t *raw_frame[2];
static uint16_t *rgb_frame;
static unsigned raw_index;
static bool display_started;
static unsigned frame_count;
static unsigned frame_changes;
static unsigned last_frame_hash;
static emu_clock lynx_clock;

static void log_error(const char *kind, const char *message)
{
    if (_papp_svc && _papp_svc->log_printf)
        _papp_svc->log_printf("Handy %s: %s\n", kind, message ? message : "(null)");
}

class PappError final : public CErrorInterface {
public:
    int Warning(const char *message) override { log_error("warning", message); return 0; }
    int Fatal(const char *message) override { log_error("fatal", message); return 0; }
};

static PappError papp_error;

extern "C" void *my_special_alloc(unsigned char speed, unsigned char bytes,
                                   unsigned long size)
{
    (void)speed;
    (void)bytes;
    void *p = _papp_svc->mem_caps_alloc(size, PAPP_MEM_CAP_SPIRAM);
    if (!p) p = _papp_svc->mem_alloc(size);
    if (p) memset(p, 0, size);
    return p;
}

extern "C" void my_special_alloc_free(void *p)
{
    if (p) _papp_svc->mem_free(p);
}

bool skipNextFrame = false;
ULONG *lynx_mColourMap = NULL;

extern "C" void app_return_to_launcher(void) { longjmp(exit_env, 1); }

static unsigned frame_hash(const uint16_t *pixels)
{
    unsigned h = 2166136261u;
    for (int y = 0; y < HANDY_SCREEN_HEIGHT; y += 7)
        for (int x = 0; x < HANDY_SCREEN_WIDTH; x += 11)
            h = (h ^ pixels[y * HANDY_SCREEN_WIDTH + x]) * 16777619u;
    return h;
}

/* Mikie's RAW layout is 64 bytes of sixteen 12-bit palette entries followed
 * by 80 bytes containing two 4-bit pixels per byte. The remaining bytes in
 * each 320-byte line are padding so the core can use a 320-byte pitch. */
static void decode_and_submit(const uint8_t *raw)
{
    for (int y = 0; y < HANDY_SCREEN_HEIGHT; y++) {
        const uint8_t *line = raw + y * HANDY_SCREEN_WIDTH * 2;
        uint16_t lut[16];
        const uint32_t *palette = (const uint32_t *)line;
        for (int i = 0; i < 16; i++) {
            uint32_t index = palette[i];
            unsigned green = index & 0x0f;
            unsigned red = (index >> 4) & 0x0f;
            unsigned blue = (index >> 8) & 0x0f;
            lut[i] = (uint16_t)(((red << 12) & 0xf800) |
                                ((green << 7) & 0x07e0) |
                                ((blue << 1) & 0x001f));
        }

        const uint8_t *packed = line + 64;
        uint16_t *out = rgb_frame + y * HANDY_SCREEN_WIDTH;
        for (int x = 0; x < HANDY_SCREEN_WIDTH; x += 2) {
            uint8_t pair = packed[x >> 1];
            out[x] = lut[pair >> 4];
            out[x + 1] = lut[pair & 0x0f];
        }
    }

    _papp_svc->display_write_frame_custom(rgb_frame, HANDY_SCREEN_WIDTH,
                                          HANDY_SCREEN_HEIGHT, 2.0f, false);

    if (gAudioBufferPointer) {
        unsigned bytes = gAudioBufferPointer;
        unsigned samples = bytes / 4; /* Handy stores signed 16-bit stereo. */
        if (samples) _papp_svc->audio_submit((short *)gAudioBuffer, (int)samples);
        gAudioBufferPointer = 0;
    }

    unsigned hash = frame_hash(rgb_frame);
    if (frame_count && hash != last_frame_hash) ++frame_changes;
    last_frame_hash = hash;
    ++frame_count;
}

static UBYTE *display_callback(ULONG object_reference)
{
    (void)object_reference;
    if (display_started) {
        decode_and_submit(raw_frame[raw_index]);
        raw_index ^= 1u;
    } else {
        display_started = true;
    }
    return raw_frame[raw_index];
}

static ULONG input_buttons(const papp_gamepad_state_t *pad)
{
    ULONG buttons = 0;
    if (pad->values[PAPP_INPUT_A])      buttons |= BUTTON_A;
    if (pad->values[PAPP_INPUT_B])      buttons |= BUTTON_B;
    if (pad->values[PAPP_INPUT_LEFT])  buttons |= BUTTON_LEFT;
    if (pad->values[PAPP_INPUT_RIGHT]) buttons |= BUTTON_RIGHT;
    if (pad->values[PAPP_INPUT_UP])    buttons |= BUTTON_UP;
    if (pad->values[PAPP_INPUT_DOWN])  buttons |= BUTTON_DOWN;
    if (pad->values[PAPP_INPUT_START]) buttons |= BUTTON_PAUSE;
    if (pad->values[PAPP_INPUT_SELECT]) buttons |= BUTTON_OPT1;
    return buttons;
}

static void release_resources(void)
{
    if (lynx_system) {
        lynx_system->SaveEEPROM();
        delete lynx_system;
        lynx_system = NULL;
    }
    for (unsigned i = 0; i < 2; i++) {
        if (raw_frame[i]) {
            _papp_svc->mem_free(raw_frame[i]);
            raw_frame[i] = NULL;
        }
    }
    if (rgb_frame) {
        _papp_svc->mem_free(rgb_frame);
        rgb_frame = NULL;
    }
    _papp_svc->audio_init(0);
}

/* The PAPP header uses entry_off=0. Keep the C++ entry point at the first
 * byte of the flat image, just like the C PAPP entry points. */
extern "C" __attribute__((section(".text.entry"), used))
int app_entry(const app_services_t *svc)
{
    if (!svc || svc->abi_version != PAPP_ABI_VERSION) return -1;
    _papp_svc = svc;
    frame_count = 0;
    frame_changes = 0;
    last_frame_hash = 0;
    raw_index = 0;
    display_started = false;
    lynx_system = NULL;
    raw_frame[0] = raw_frame[1] = NULL;
    rgb_frame = NULL;

    char rom_path[512];
    if (emu_rom_path("handy", ".lnx|.lyx|", rom_path, sizeof(rom_path))) {
        svc->log_printf("Handy: set /sd/roms/papp/handy.rom to an .lnx or .lyx ROM\n");
        return -1;
    }

    int result = -1;
    if (setjmp(exit_env)) goto cleanup;

    for (unsigned i = 0; i < 2; i++) {
        raw_frame[i] = (uint8_t *)svc->mem_caps_alloc(
            HANDY_SCREEN_WIDTH * HANDY_SCREEN_HEIGHT * 2,
            PAPP_MEM_CAP_SPIRAM);
        if (!raw_frame[i]) app_return_to_launcher();
        memset(raw_frame[i], 0,
               HANDY_SCREEN_WIDTH * HANDY_SCREEN_HEIGHT * 2);
    }
    rgb_frame = (uint16_t *)svc->mem_caps_alloc(
        HANDY_SCREEN_WIDTH * HANDY_SCREEN_HEIGHT * sizeof(uint16_t),
        PAPP_MEM_CAP_SPIRAM);
    if (!rgb_frame) app_return_to_launcher();
    memset(rgb_frame, 0,
           HANDY_SCREEN_WIDTH * HANDY_SCREEN_HEIGHT * sizeof(uint16_t));

    /* Handy accepts an empty boot-ROM path and uses its built-in HLE BIOS. */
    gError = &papp_error;
    gAudioEnabled = TRUE;
    gAudioBufferPointer = 0;
    svc->log_printf("Handy PAPP: initializing %s\n", rom_path);
    svc->audio_init(HANDY_AUDIO_SAMPLE_FREQ);
    lynx_system = new CSystem(rom_path, "", true);
    if (!lynx_system) app_return_to_launcher();

    lynx_system->DisplaySetAttributes(MIKIE_NO_ROTATE,
                                      MIKIE_PIXEL_FORMAT_16BPP_565,
                                      HANDY_SCREEN_WIDTH * 2,
                                      display_callback, 0);
    svc->log_printf("Handy PAPP: running %s\n", rom_path);

    lynx_clock.deadline = svc->get_time_us();
    lynx_clock.phase = 0;
    lynx_clock.hz = 75;
    for (;;) {
        papp_gamepad_state_t pad;
        svc->input_gamepad_read(&pad);
        if (emu_quit(&pad)) {
            svc->log_printf("Handy PAPP: quit input menu=%d l3=%d frames=%u\n",
                            pad.values[PAPP_INPUT_MENU],
                            svc->input_l3_read ? svc->input_l3_read() : 0,
                            frame_count);
            break;
        }
        lynx_system->SetButtonData(input_buttons(&pad));

        unsigned before = frame_count;
        while (frame_count == before) lynx_system->Update();
        emu_pace(&lynx_clock);

#ifdef PAPP_TEST_FRAMES
        if (frame_count >= PAPP_TEST_FRAMES) break;
#endif
    }

    svc->log_printf("Handy PAPP: frames=%u changing=%u\n",
                    frame_count, frame_changes);
    result = 0;

cleanup:
    release_resources();
    return result;
}
