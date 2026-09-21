/* Neo Geo Pocket / Pocket Color PAPP wrapper for Race. */
#define PAPP_APP_SIDE 1

#include "runtime.h"
#include "types.h"
#define RETRO_COMPAT_IMPLEMENTATION
#include "retro_compat.h"
#include "race-memory.h"
#include "graphics.h"
#include "input.h"
#include "tlcs900h.h"
#include "flash.h"
#include "neopopsound.h"

#include <stdint.h>
#include <stddef.h>
#include <setjmp.h>
#include <string.h>

const app_services_t *_papp_svc;

/* Race's renderer is intentionally hardware-neutral in this tree. */
unsigned short *drawBuffer = NULL;
volatile unsigned g_frame_ready = 0;
unsigned char *rasterY = NULL;
unsigned char *frame0Pri = NULL;
unsigned char *frame1Pri = NULL;
unsigned char *color_switch = NULL;
unsigned char *scanlineY = NULL;
unsigned char *scrollSpriteX = NULL;
unsigned char *scrollSpriteY = NULL;
unsigned char *sprite_palette_numbers = NULL;
unsigned char *sprite_table = NULL;
unsigned short *patterns = NULL;
unsigned char *oowSelect = NULL;
unsigned short *oowTable = NULL;
unsigned char *wndTopLeftY = NULL;
unsigned char *wndSizeY = NULL;
unsigned char *wndSizeX = NULL;
unsigned char *bgSelect = NULL;
unsigned char *bw_palette_table = NULL;
unsigned short *palette_table = NULL;
unsigned short *bgTable = NULL;
unsigned char *wndTopLeftX = NULL;
unsigned char *scrollFrontY = NULL;
unsigned char *scrollFrontX = NULL;
unsigned short *tile_table_front = NULL;
unsigned char *scrollBackY = NULL;
unsigned short *tile_table_back = NULL;
unsigned char *scrollBackX = NULL;
unsigned char *pattern_table = NULL;

int m_bIsActive = 1;
int gfx_hacks = 0;
int tipo_consola = 0;
char retro_save_directory[3] = "/";

static struct ngp_screen s_screen;
static uint16_t *s_audio_psg;
static uint16_t *s_audio_dac;
static int16_t *s_audio_mix;
static jmp_buf s_exit_env;

extern "C" int Cz80_allocate_flag_tables(void);
extern "C" void audio_dac_init(void);
extern "C" void tlcs_reset(void) __attribute__((weak));
extern "C" void tlcs_reset(void) {}

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

static void map_vdp_tables(void)
{
    sprite_table           = (unsigned char *)get_address(0x00008800);
    pattern_table          = (unsigned char *)get_address(0x0000A000);
    patterns               = (unsigned short *)pattern_table;
    tile_table_front       = (unsigned short *)get_address(0x00009000);
    tile_table_back        = (unsigned short *)get_address(0x00009800);
    palette_table          = (unsigned short *)get_address(0x00008200);
    bw_palette_table       = (unsigned char *)get_address(0x00008100);
    sprite_palette_numbers = (unsigned char *)get_address(0x00008C00);
    scanlineY              = (unsigned char *)get_address(0x00008009);
    frame0Pri              = (unsigned char *)get_address(0x00008000);
    frame1Pri              = (unsigned char *)get_address(0x00008030);
    wndTopLeftX            = (unsigned char *)get_address(0x00008002);
    wndTopLeftY            = (unsigned char *)get_address(0x00008003);
    wndSizeX               = (unsigned char *)get_address(0x00008004);
    wndSizeY               = (unsigned char *)get_address(0x00008005);
    scrollSpriteX          = (unsigned char *)get_address(0x00008020);
    scrollSpriteY          = (unsigned char *)get_address(0x00008021);
    scrollFrontX           = (unsigned char *)get_address(0x00008032);
    scrollFrontY           = (unsigned char *)get_address(0x00008033);
    scrollBackX            = (unsigned char *)get_address(0x00008034);
    scrollBackY            = (unsigned char *)get_address(0x00008035);
    bgSelect               = (unsigned char *)get_address(0x00008118);
    bgTable                = (unsigned short *)get_address(0x000083E0);
    oowSelect              = (unsigned char *)get_address(0x00008012);
    oowTable               = (unsigned short *)get_address(0x000083F0);
    color_switch           = (unsigned char *)get_address(0x00006F91);
    rasterY                = scanlineY;
}

static void set_boot_defaults(void)
{
    tlcsMemWriteB(0x00006F91, tlcsMemReadB(0x00200023));
    if (tipo_consola == 1) tlcsMemWriteB(0x00006F91, 0x00);
    tlcsMemWriteB(0x00006F87, 0x01); /* English */
    tlcsMemWriteB(0x00004000, tlcsMemReadB(0x00004000) | 0xC0);
    tlcsMemWriteB(0x00006F84, 0x40);
    tlcsMemWriteB(0x00006F85, 0x00);
    tlcsMemWriteB(0x00006F86, 0x00);
}

/* Race calls this after the last visible scanline. */
extern "C" void graphics_paint(unsigned char render)
{
    if (!render || !drawBuffer || !_papp_svc ||
        !_papp_svc->display_write_frame_custom) return;
    _papp_svc->display_write_frame_custom(drawBuffer, 160, 152, 3.0f, false);
}

static void update_input(void)
{
    papp_gamepad_state_t pad;
    memset(&pad, 0, sizeof(pad));
    _papp_svc->input_gamepad_read(&pad);
    ngpInputState = 0;
    if (pad.values[PAPP_INPUT_UP])     ngpInputState |= 1u << 0;
    if (pad.values[PAPP_INPUT_DOWN])   ngpInputState |= 1u << 1;
    if (pad.values[PAPP_INPUT_LEFT])   ngpInputState |= 1u << 2;
    if (pad.values[PAPP_INPUT_RIGHT])  ngpInputState |= 1u << 3;
    if (pad.values[PAPP_INPUT_A])      ngpInputState |= 1u << 4;
    if (pad.values[PAPP_INPUT_B])      ngpInputState |= 1u << 5;
    if (pad.values[PAPP_INPUT_START] || pad.values[PAPP_INPUT_SELECT])
        ngpInputState |= 1u << 6;
    if (emu_quit(&pad)) longjmp(s_exit_env, 1);
}

static void submit_audio(void)
{
    enum { kRate = 22050, kSamples = 368 };
    sound_update(s_audio_psg, kSamples * (int)sizeof(uint16_t));
    dac_update(s_audio_dac, kSamples * (int)sizeof(uint16_t));
    for (int i = 0; i < kSamples; ++i) {
        int32_t v = (int16_t)s_audio_psg[i] + (int16_t)s_audio_dac[i];
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        /* PAPP audio_submit consumes interleaved stereo frames.  Race's
         * native mixer is mono, so duplicate each sample for the two output
         * channels rather than treating the mono buffer as stereo data. */
        s_audio_mix[2 * i] = (int16_t)v;
        s_audio_mix[2 * i + 1] = (int16_t)v;
    }
    _papp_svc->audio_submit(s_audio_mix, kSamples);
}

extern "C" __attribute__((section(".text.entry"), used))
int app_entry(const app_services_t *svc)
{
    if (!svc || svc->abi_version != PAPP_ABI_VERSION) return -1;
    _papp_svc = svc;
    drawBuffer = NULL;
    s_audio_psg = s_audio_dac = NULL;
    s_audio_mix = NULL;
    m_bIsActive = 1;

    uint8_t *rom = NULL;
    size_t rom_size = 0;
    char rom_path[512];
    if (emu_load_rom("ngp", ".ngp|.ngc|", &rom, &rom_size,
                     rom_path, sizeof(rom_path))) {
        svc->log_printf("NGP PAPP: set /sd/roms/papp/ngp.rom to a .ngp or .ngc ROM\n");
        return -1;
    }

    const char *dot = strrchr(rom_path, '.');
    tipo_consola = (dot && (dot[1] == 'n' || dot[1] == 'N') &&
                    (dot[2] == 'g' || dot[2] == 'G') &&
                    (dot[3] == 'p' || dot[3] == 'P')) ? 1 : 0;

    /* Race's sprite/cache code uses the historical 260-pixel stride even
     * though the final LCD image is 160 pixels wide. */
    s_screen.w = 260;
    s_screen.h = 152;
    s_screen.pixels = svc->mem_caps_alloc(260u * 152u * sizeof(uint16_t),
                                          PAPP_MEM_CAP_SPIRAM);
    s_audio_psg = (uint16_t *)svc->mem_caps_alloc(368u * sizeof(uint16_t),
                                                  PAPP_MEM_CAP_SPIRAM);
    s_audio_dac = (uint16_t *)svc->mem_caps_alloc(368u * sizeof(uint16_t),
                                                  PAPP_MEM_CAP_SPIRAM);
    s_audio_mix = (int16_t *)svc->mem_caps_alloc(368u * 2u * sizeof(int16_t),
                                                 PAPP_MEM_CAP_SPIRAM);
    if (!s_screen.pixels || !s_audio_psg || !s_audio_dac || !s_audio_mix)
        goto cleanup;
    memset(s_screen.pixels, 0, 260u * 152u * sizeof(uint16_t));
    memset(s_audio_psg, 0, 368u * sizeof(uint16_t));
    memset(s_audio_dac, 0, 368u * sizeof(uint16_t));

    if (setjmp(s_exit_env)) goto cleanup_core;

    screen = &s_screen;
    drawBuffer = (unsigned short *)s_screen.pixels;
    svc->log_printf("NGP PAPP: loading %s (%u bytes)\n",
                    rom_path, (unsigned)rom_size);
    svc->audio_init(22050);
    ngp_mem_set_rom(rom, rom_size);
    setFlashSize((unsigned)rom_size);
    m_emuInfo.machine = tipo_consola ? NGP : NGPC;
    m_emuInfo.romSize = (int)rom_size;

    Cz80_allocate_flag_tables();
    ngp_mem_init();
    map_vdp_tables();
    if (bgTable) bgTable[0] = 0xFFFF;
    if (bgSelect) *bgSelect |= 0x80;
    tlcs_init();
    tlcs_reset();
    Z80_Init();
    Z80_Reset();
    set_boot_defaults();
    graphics_init();
    if (wndSizeX && wndSizeY && (*wndSizeX == 0 || *wndSizeY == 0)) {
        *wndTopLeftX = 0; *wndTopLeftY = 0;
        *wndSizeX = 160; *wndSizeY = 152;
    }
    if (frame0Pri) *frame0Pri |= 0xC0;
    tlcsMemWriteB(0x00004000, 0xC0);
    audio_dac_init();
    sound_init(22050);

    {
        emu_clock clock = { svc->get_time_us(), 0, 60 };
        const uint32_t cycles = 5700000u / 60u;
        unsigned frames = 0;
        svc->log_printf("NGP PAPP: running %s\n", rom_path);
        for (;;) {
            update_input();
            tlcs_execute((int)cycles);
            graphics_paint(1);
            submit_audio();
            emu_pace(&clock);
            if ((++frames & 127u) == 0) svc->log_printf("NGP PAPP: frames=%u\n", frames);
#ifdef PAPP_TEST_FRAMES
            if (frames >= PAPP_TEST_FRAMES) break;
#endif
        }
    }

cleanup_core:
    flashShutdown();
    ngp_mem_free();
cleanup:
    if (s_screen.pixels) svc->mem_free(s_screen.pixels);
    if (s_audio_psg) svc->mem_free(s_audio_psg);
    if (s_audio_dac) svc->mem_free(s_audio_dac);
    if (s_audio_mix) svc->mem_free(s_audio_mix);
    if (rom) svc->mem_free(rom);
    s_screen.pixels = NULL;
    drawBuffer = NULL;
    return 0;
}
