/* MSX cartridge PAPP wrapper for the fMSX core. */
#define PAPP_APP_SIDE 1

#include "runtime.h"

extern "C" {
#include "fMSX/MSX.h"
#include "EMULib/Sound.h"
}

#include "msx_host.h"
#include "msx_cbios_msx1.h"

#include <stdint.h>
#include <stddef.h>
#include <setjmp.h>
#include <string.h>
#include <strings.h>

const app_services_t *_papp_svc;

/* BPP8 keeps the fMSX render path small: XBuf contains palette indices and
 * this wrapper converts them to the launcher RGB565 frame format once per
 * frame. */
extern "C" pixel *XPal;
extern "C" pixel *BPal;
extern "C" pixel XPal0;
extern "C" pixel *XBuf;
pixel *XPal = NULL;
pixel *BPal = NULL;
pixel XPal0 = 0;
pixel *XBuf = NULL;

static const uint8_t *s_rom;
static unsigned int s_rom_size;
static char s_rom_path[512];
static uint8_t *s_frame8;
static uint16_t *s_frame565;
static int16_t *s_audio;
static uint16_t s_palette[256];
static jmp_buf s_exit_env;
static uint8_t *s_msx2_main;
static uint8_t *s_msx2_ext;

static uint8_t *load_host_file(const char *const *paths, size_t path_count,
                               size_t expected_size)
{
    if (!_papp_svc || !_papp_svc->file_open || !_papp_svc->file_read ||
        !_papp_svc->file_close || !_papp_svc->mem_caps_alloc) return NULL;
    for (size_t i = 0; i < path_count; ++i) {
        void *f = _papp_svc->file_open(paths[i], "rb");
        if (!f) continue;
        uint8_t *data = (uint8_t *)_papp_svc->mem_caps_alloc(
            expected_size, PAPP_MEM_CAP_SPIRAM);
        size_t got = data ? _papp_svc->file_read(data, 1, expected_size, f) : 0;
        _papp_svc->file_close(f);
        if (data && got == expected_size) {
            _papp_svc->log_printf("MSX PAPP: using BIOS %s\n", paths[i]);
            return data;
        }
        if (data) _papp_svc->mem_free(data);
    }
    return NULL;
}

extern "C" void PutImage(void);
extern "C" int msx_line_render_enabled(void) { return 0; }
extern "C" pixel *msx_line_begin(int, pixel, int) { return NULL; }
extern "C" void msx_line_end_frame(void) {}

/* The fMSX core keeps legacy cwd hooks even when ProgDir is NULL.  PAPPs do
 * not expose a process working directory, so these are harmless stubs. */
extern "C" int chdir(const char *) { return 0; }
extern "C" char *getcwd(char *, size_t) { return NULL; }

extern "C" {
#define NARROW
#define WIDTH 272
#define HEIGHT 228
#include "fMSX/Common.h"
#undef NARROW
#undef WIDTH
#undef HEIGHT
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

static uint16_t rgb565(unsigned r, unsigned g, unsigned b)
{
    return (uint16_t)(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3));
}

extern "C" void msx_host_set_game(const uint8_t *rom_data,
                                    unsigned int rom_size,
                                    const char *, const char *rom_path)
{
    s_rom = rom_data;
    s_rom_size = rom_size;
    if (rom_path) {
        strncpy(s_rom_path, rom_path, sizeof(s_rom_path) - 1);
        s_rom_path[sizeof(s_rom_path) - 1] = '\0';
    } else {
        s_rom_path[0] = '\0';
    }
}

extern "C" void msx_host_clear_game(void)
{
    s_rom = NULL;
    s_rom_size = 0;
    s_rom_path[0] = '\0';
}

extern "C" int msx_host_load_bios_for_mode(int mode)
{
    const int model = mode & MSX_MODEL;
    if (model == MSX_MSX1) return 1;
    if (model != MSX_MSX2) return 0;

    static const char *const main_paths[] = {
        "/sd/roms/papp/MSX2.ROM", "/sd/roms/papp/msx2.rom",
        "/sd/roms/msx/MSX2.ROM", "/sd/roms/system/MSX2.ROM"
    };
    static const char *const ext_paths[] = {
        "/sd/roms/papp/MSX2EXT.ROM", "/sd/roms/papp/msx2ext.rom",
        "/sd/roms/msx/MSX2EXT.ROM", "/sd/roms/system/MSX2EXT.ROM"
    };
    s_msx2_main = load_host_file(main_paths,
                                 sizeof(main_paths) / sizeof(main_paths[0]),
                                 0x8000u);
    s_msx2_ext = load_host_file(ext_paths,
                                sizeof(ext_paths) / sizeof(ext_paths[0]),
                                0x4000u);
    if (!s_msx2_main || !s_msx2_ext) {
        if (_papp_svc) _papp_svc->log_printf(
            "MSX PAPP: MSX2 needs MSX2.ROM (32 KiB) and MSX2EXT.ROM (16 KiB)\n");
        msx_host_unload_bios();
        return 0;
    }
    return 1;
}

extern "C" void msx_host_unload_bios(void)
{
    if (_papp_svc) {
        if (s_msx2_main) _papp_svc->mem_free(s_msx2_main);
        if (s_msx2_ext) _papp_svc->mem_free(s_msx2_ext);
    }
    s_msx2_main = NULL;
    s_msx2_ext = NULL;
}

extern "C" const uint8_t *msx_host_get_builtin_file(const char *name,
                                                     unsigned int *size)
{
    if (size) *size = 0;
    if (!name) return NULL;
    if (strcasecmp(name, "MSX.ROM") == 0) {
        if (size) *size = kMsxCbiosMainMsx1RomLen;
        return kMsxCbiosMainMsx1Rom;
    }
    if (strcasecmp(name, "MSX2.ROM") == 0 && s_msx2_main) {
        if (size) *size = 0x8000u;
        return s_msx2_main;
    }
    if (strcasecmp(name, "MSX2EXT.ROM") == 0 && s_msx2_ext) {
        if (size) *size = 0x4000u;
        return s_msx2_ext;
    }
    return NULL;
}

extern "C" int msx_host_is_cbios_fallback_active(void)
{
    return s_msx2_main == NULL;
}

extern "C" const uint8_t *msx_host_get_mapped_rom(const char *file_name,
                                                   unsigned int *size)
{
    if (size) *size = 0;
    if (!s_rom || !s_rom_size || !file_name) return NULL;
    const char *base = strrchr(file_name, '/');
    base = base ? base + 1 : file_name;
    if (strcasecmp(base, "CARTA.ROM") != 0 &&
        (s_rom_path[0] == '\0' || strcasecmp(base, strrchr(s_rom_path, '/') ?
                                                strrchr(s_rom_path, '/') + 1 :
                                                s_rom_path) != 0)) {
        return NULL;
    }
    if (size) *size = s_rom_size;
    return s_rom;
}

extern "C" int msx_host_prepare_runtime(void)
{
    if (!s_frame8) {
        s_frame8 = (uint8_t *)_papp_svc->mem_caps_alloc(
            272u * 228u, PAPP_MEM_CAP_SPIRAM);
    }
    if (!s_frame565) {
        s_frame565 = (uint16_t *)_papp_svc->mem_caps_alloc(
            272u * 228u * sizeof(uint16_t), PAPP_MEM_CAP_SPIRAM);
    }
    if (!s_audio) {
        s_audio = (int16_t *)_papp_svc->mem_caps_alloc(
            512u * 2u * sizeof(int16_t), PAPP_MEM_CAP_SPIRAM);
    }
    XBuf = (pixel *)s_frame8;
    return s_frame8 && s_frame565 && s_audio;
}

extern "C" void msx_save_set_game_identity(const char *, const char *)
{
}
extern "C" void msx_save_clear_game_identity(void)
{
}
extern "C" int msx_save_build_path(char *, unsigned int, const char *)
{
    return 0;
}

extern "C" void SetColor(byte n, byte r, byte g, byte b)
{
    if (n >= 256) return;
    s_palette[n] = rgb565(r, g, b);
    if (XPal && n < 80) XPal[n] = n;
    if (BPal && n < 256) BPal[n] = n;
}

extern "C" int InitMachine(void)
{
    if (!_papp_svc) return 0;
    if (!XPal) XPal = (pixel *)_papp_svc->mem_caps_alloc(80, PAPP_MEM_CAP_INTERNAL);
    if (!BPal) BPal = (pixel *)_papp_svc->mem_caps_alloc(256, PAPP_MEM_CAP_INTERNAL);
    if (!XPal || !BPal) return 0;

    for (unsigned i = 0; i < 256; ++i) {
        const unsigned r = ((i >> 5) & 7u) * 255u / 7u;
        const unsigned g = ((i >> 2) & 7u) * 255u / 7u;
        const unsigned b = (i & 3u) * 255u / 3u;
        s_palette[i] = rgb565(r, g, b);
        BPal[i] = (pixel)i;
    }
    for (unsigned i = 0; i < 80; ++i) XPal[i] = (pixel)i;
    XPal0 = 0;
    return 1;
}

extern "C" void TrashMachine(void)
{
    XBuf = NULL;
    if (XPal) _papp_svc->mem_free(XPal);
    if (BPal) _papp_svc->mem_free(BPal);
    XPal = NULL;
    BPal = NULL;
}

extern "C" void PutImage(void)
{
    if (!XBuf || !s_frame565 || !_papp_svc ||
        !_papp_svc->display_write_frame_custom) return;
    static unsigned frame_count;
    unsigned nonzero_indices = 0;
    unsigned nonblack_pixels = 0;
    unsigned min_index = 255;
    unsigned max_index = 0;
    for (unsigned i = 0; i < 272u * 228u; ++i) {
        const unsigned index = (unsigned)(unsigned char)XBuf[i];
        const uint16_t color = s_palette[index];
        s_frame565[i] = color;
        nonzero_indices += index != 0;
        nonblack_pixels += color != 0;
        if (index < min_index) min_index = index;
        if (index > max_index) max_index = index;
    }
    ++frame_count;
    if (frame_count <= 3u || (frame_count % 120u) == 0u) {
        _papp_svc->log_printf(
            "MSX framebuffer #%u: indexed_nonzero=%u rgb565_nonblack=%u range=%u..%u palette=%04x,%04x,%04x vdp1=%02x mode=%u bg=%u fg=%u pc=%04x\n",
            frame_count, nonzero_indices, nonblack_pixels, min_index, max_index,
            s_palette[0], s_palette[1], s_palette[15], VDP[1], ScrMode,
            BGColor, FGColor, CPU.PC.W);
    }
    _papp_svc->display_write_frame_custom(s_frame565, 272, 228, 2.0f, false);
}

extern "C" void PlayAllSound(int usec)
{
    if (usec <= 0 || !_papp_svc || !_papp_svc->audio_submit) return;
    const unsigned samples = (unsigned)((uint64_t)usec * 44100u / 1000000u);
    RenderAndPlayAudio(samples > 512u ? 512u : samples);
}

extern "C" unsigned int InitAudio(unsigned int rate, unsigned int)
{
    if (_papp_svc && _papp_svc->audio_init) _papp_svc->audio_init((int)rate);
    return rate;
}

extern "C" void TrashAudio(void)
{
}

extern "C" unsigned int GetFreeAudio(void)
{
    return 512;
}

extern "C" unsigned int WriteAudio(sample *data, unsigned int length)
{
    if (!data || !s_audio || !_papp_svc || !_papp_svc->audio_submit) return 0;
    if (length > 512) length = 512;
    for (unsigned i = 0; i < length; ++i) {
        s_audio[2 * i] = data[i];
        s_audio[2 * i + 1] = data[i];
    }
    _papp_svc->audio_submit(s_audio, (int)length);
    return length;
}

static void poll_pad(void)
{
    papp_gamepad_state_t pad;
    memset(&pad, 0, sizeof(pad));
    _papp_svc->input_gamepad_read(&pad);
    memset((void *)KeyState, 0xFF, sizeof(KeyState));
    if (pad.values[PAPP_INPUT_LEFT])  KBD_SET(KBD_LEFT);
    if (pad.values[PAPP_INPUT_RIGHT]) KBD_SET(KBD_RIGHT);
    if (pad.values[PAPP_INPUT_UP])    KBD_SET(KBD_UP);
    if (pad.values[PAPP_INPUT_DOWN])  KBD_SET(KBD_DOWN);
    if (pad.values[PAPP_INPUT_A])     KBD_SET(KBD_SPACE);
    if (pad.values[PAPP_INPUT_B])     KBD_SET(KBD_ENTER);
    if (emu_quit(&pad)) ExitNow = 1;
}

extern "C" unsigned int Joystick(void)
{
    poll_pad();
    papp_gamepad_state_t pad;
    memset(&pad, 0, sizeof(pad));
    _papp_svc->input_gamepad_read(&pad);
    unsigned int joy = 0;
    if (pad.values[PAPP_INPUT_UP])    joy |= JST_UP;
    if (pad.values[PAPP_INPUT_DOWN])  joy |= JST_DOWN;
    if (pad.values[PAPP_INPUT_LEFT])  joy |= JST_LEFT;
    if (pad.values[PAPP_INPUT_RIGHT]) joy |= JST_RIGHT;
    if (pad.values[PAPP_INPUT_A])     joy |= JST_FIREA;
    if (pad.values[PAPP_INPUT_B])     joy |= JST_FIREB;
    return joy;
}

extern "C" void Keyboard(void)
{
}

extern "C" unsigned int Mouse(byte)
{
    return 0;
}

extern "C" __attribute__((section(".text.entry"), used))
int app_entry(const app_services_t *svc)
{
    if (!svc || svc->abi_version != PAPP_ABI_VERSION) return -1;
    _papp_svc = svc;
    s_rom = NULL;
    s_rom_size = 0;
    s_frame8 = NULL;
    s_frame565 = NULL;
    s_audio = NULL;
    s_msx2_main = NULL;
    s_msx2_ext = NULL;

    uint8_t *rom = NULL;
    size_t rom_size = 0;
    char rom_path[512];
    if (emu_load_rom("msx", ".rom|.mx1|.mx2|", &rom, &rom_size,
                     rom_path, sizeof(rom_path))) {
        svc->log_printf("MSX PAPP: set /sd/roms/papp/msx.rom to an MSX ROM\n");
        return -1;
    }
    msx_host_set_game(rom, (unsigned int)rom_size, rom_path, rom_path);
    const char *ext = strrchr(rom_path, '.');
    const int is_msx2 = ext && strcasecmp(ext, ".mx2") == 0;
    const int mode = (is_msx2 ? MSX_MSX2 : MSX_MSX1) |
                     MSX_NTSC | MSX_JOY1 | MSX_GUESSA;
    if (!msx_host_load_bios_for_mode(mode) || !InitMachine()) goto cleanup;
    Verbose = 1;
    if (is_msx2) svc->log_printf("MSX PAPP: MSX2 mode selected\n");
    svc->log_printf("MSX PAPP: loading %s (%u bytes)\n",
                    rom_path, (unsigned)rom_size);
    if (setjmp(s_exit_env)) goto cleanup_core;
    if (!StartMSX(mode, 4, 2)) {
        svc->log_printf("MSX PAPP: core initialization failed\n");
        goto cleanup_core;
    }
    svc->log_printf("MSX PAPP: running %s\n", rom_path);

cleanup_core:
    TrashMSX();
    TrashMachine();
cleanup:
    msx_host_unload_bios();
    if (s_frame8) svc->mem_free(s_frame8);
    if (s_frame565) svc->mem_free(s_frame565);
    if (s_audio) svc->mem_free(s_audio);
    s_frame8 = NULL;
    s_frame565 = NULL;
    s_audio = NULL;
    msx_host_clear_game();
    if (rom) svc->mem_free(rom);
    return 0;
}
