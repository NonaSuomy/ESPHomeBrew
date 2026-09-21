/* GX4000 / Amstrad CPC+ cartridge PAPP wrapper for Arnold. */
#define PAPP_APP_SIDE 1

#include "runtime.h"
#include "gx4000_display.h"

extern "C" {
#include "arnold/cpcglob.h"
#include "arnold/cpc.h"
#include "arnold/arnold.h"
#include "arnold/asic.h"
#include "arnold/render.h"
#include "arnold/garray.h"
#include "arnold/host.h"
#include "arnold/audioevent.h"
}

#include <stdint.h>
#include <stddef.h>
#include <setjmp.h>
#include <string.h>

const app_services_t *_papp_svc;
static uint16_t *s_frame;
static jmp_buf s_exit_env;
static GRAPHICS_BUFFER_COLOUR_FORMAT s_colour_fmt = {
    16, {5, 0xF800, 11}, {6, 0x07E0, 5}, {5, 0x001F, 0}
};
static GRAPHICS_BUFFER_INFO s_buffer_info;
static SOUND_PLAYBACK_FORMAT s_sound_fmt = {2, 16, 22050};

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

extern "C" void gx4000_display_init(void)
{
}

extern "C" void gx4000_display_shutdown(void)
{
}

/* Arnold's line renderer already produces RGB565.  Accumulate the useful
 * visible window in PSRAM and submit one complete frame when Arnold signals
 * the end of a monitor frame.  This keeps LCD I/O out of the scanline path. */
extern "C" void gx4000_display_flush_line(int vis_y,
                                            const unsigned char *data,
                                            int x_offset, int render_w,
                                            int bytes_per_pixel)
{
    if (!s_frame || !data || bytes_per_pixel != 2 ||
        vis_y < 0 || vis_y >= GX4000_CPC_VISIBLE_H || render_w <= 0) return;
    if (render_w > GX4000_CPC_VISIBLE_W) render_w = GX4000_CPC_VISIBLE_W;
    const uint16_t *src = (const uint16_t *)data + x_offset;
    uint16_t *dst = s_frame + (size_t)vis_y * GX4000_CPC_VISIBLE_W;
    for (int x = 0; x < render_w; ++x) dst[x] = src[x];
    for (int x = render_w; x < GX4000_CPC_VISIBLE_W; ++x) dst[x] = 0;
}

extern "C" void gx4000_display_frame_done(void)
{
    if (_papp_svc && _papp_svc->display_write_frame_custom && s_frame) {
        _papp_svc->display_write_frame_custom(
            s_frame, GX4000_CPC_VISIBLE_W, GX4000_CPC_VISIBLE_H,
            1.25f, false);
    }
}

extern "C" unsigned long gx4000_display_take_drop_count(void)
{
    return 0;
}
extern "C" void gx4000_display_cycle_view(void) {}
extern "C" void gx4000_display_zoom_in(void) {}
extern "C" void gx4000_display_zoom_out(void) {}

extern "C" void Host_SetGraphicsBufferSurface(unsigned char *surface,
                                                int width, int height,
                                                int pitch)
{
    s_buffer_info.pSurface = surface;
    s_buffer_info.Width = width;
    s_buffer_info.Height = height;
    s_buffer_info.Pitch = pitch;
}

extern "C" GRAPHICS_BUFFER_COLOUR_FORMAT *Host_GetGraphicsBufferColourFormat(void)
{
    return &s_colour_fmt;
}
extern "C" GRAPHICS_BUFFER_INFO *Host_GetGraphicsBufferInfo(void)
{
    return &s_buffer_info;
}
extern "C" BOOL Host_LockGraphicsBuffer(void) { return TRUE; }
extern "C" void Host_UnlockGraphicsBuffer(void) {}
extern "C" void Host_SwapGraphicsBuffers(void)
{
    gx4000_display_frame_done();
}
extern "C" void Host_SetPaletteEntry(int, unsigned char, unsigned char, unsigned char) {}
extern "C" BOOL Host_SetDisplay(int, int, int, int) { return TRUE; }
extern "C" void Host_HandlePrinterOutput(void) {}
extern "C" void Host_Throttle(void) {}

extern "C" BOOL Host_AudioPlaybackPossible(void) { return TRUE; }
extern "C" SOUND_PLAYBACK_FORMAT *Host_GetSoundPlaybackFormat(void)
{
    return &s_sound_fmt;
}
extern "C" void Host_WriteDataToSoundBuffer(unsigned char *data,
                                              unsigned long length)
{
    if (!_papp_svc || !_papp_svc->audio_submit || !data || length < 4) return;
    unsigned frames = (unsigned)(length / (2u * sizeof(int16_t)));
    if (frames > 1400u) frames = 1400u;
    _papp_svc->audio_submit((short *)data, (int)frames);
}
extern "C" BOOL Host_LockAudioBuffer(unsigned char **, unsigned long *,
                                      unsigned char **, unsigned long *, int)
{
    return FALSE;
}
extern "C" void Host_UnLockAudioBuffer(void) {}

extern "C" BOOL Host_ProcessSystemEvents(void)
{
    if (!_papp_svc || !_papp_svc->input_gamepad_read) return FALSE;
    papp_gamepad_state_t pad;
    memset(&pad, 0, sizeof(pad));
    _papp_svc->input_gamepad_read(&pad);
    if (emu_quit(&pad)) return TRUE;

    CPC_SetKey(CPC_JOY0_UP);
    CPC_SetKey(CPC_JOY0_DOWN);
    CPC_SetKey(CPC_JOY0_LEFT);
    CPC_SetKey(CPC_JOY0_RIGHT);
    CPC_SetKey(CPC_JOY0_FIRE1);
    CPC_SetKey(CPC_JOY0_FIRE2);
    if (!pad.values[PAPP_INPUT_UP])    CPC_ClearKey(CPC_JOY0_UP);
    if (!pad.values[PAPP_INPUT_DOWN])  CPC_ClearKey(CPC_JOY0_DOWN);
    if (!pad.values[PAPP_INPUT_LEFT])  CPC_ClearKey(CPC_JOY0_LEFT);
    if (!pad.values[PAPP_INPUT_RIGHT]) CPC_ClearKey(CPC_JOY0_RIGHT);
    if (!pad.values[PAPP_INPUT_A])     CPC_ClearKey(CPC_JOY0_FIRE1);
    if (!pad.values[PAPP_INPUT_B])     CPC_ClearKey(CPC_JOY0_FIRE2);
    return FALSE;
}

extern "C" unsigned long Host_GetCurrentTimeInMilliseconds(void)
{
    return _papp_svc ? (unsigned long)(_papp_svc->get_time_us() / 1000) : 0;
}

extern "C" HOST_FILE_HANDLE Host_OpenFile(const char *, int) { return 0; }
extern "C" void Host_CloseFile(HOST_FILE_HANDLE) {}
extern "C" int Host_GetFileSize(HOST_FILE_HANDLE) { return 0; }
extern "C" void Host_WriteData(HOST_FILE_HANDLE, unsigned char *, unsigned long) {}
extern "C" void Host_ReadData(HOST_FILE_HANDLE, unsigned char *, unsigned long) {}

extern "C" __attribute__((section(".text.entry"), used))
int app_entry(const app_services_t *svc)
{
    if (!svc || svc->abi_version != PAPP_ABI_VERSION) return -1;
    _papp_svc = svc;
    s_frame = NULL;

    uint8_t *rom = NULL;
    size_t rom_size = 0;
    char rom_path[512];
    if (emu_load_rom("gx4000", ".cpr|.bin|", &rom, &rom_size,
                     rom_path, sizeof(rom_path))) {
        svc->log_printf("GX4000 PAPP: set /sd/roms/papp/gx4000.rom to a .cpr or raw cart\n");
        return -1;
    }
    s_frame = (uint16_t *)svc->mem_caps_alloc(
        GX4000_CPC_VISIBLE_W * GX4000_CPC_VISIBLE_H * sizeof(uint16_t),
        PAPP_MEM_CAP_SPIRAM);
    if (!s_frame) goto cleanup;
    memset(s_frame, 0, GX4000_CPC_VISIBLE_W * GX4000_CPC_VISIBLE_H * sizeof(uint16_t));
    svc->audio_init(22050);
    svc->log_printf("GX4000 PAPP: loading %s (%u bytes)\n",
                    rom_path, (unsigned)rom_size);
    if (setjmp(s_exit_env)) goto cleanup_core;

    CPC_Initialise();
    CPC_SetFrameSkip(1);
    CPC_SetHardware(CPC_HW_CPCPLUS);
    CPC_SetRamConfig(0);
    CPC_SetCRTCType(3);
    CPC_SetMonitorType(CPC_MONITOR_COLOUR);
    if (!Render_SetDisplayFullScreen(GX4000_CPC_VISIBLE_W,
                                      GX4000_CPC_VISIBLE_H, 16)) {
        svc->log_printf("GX4000 PAPP: renderer initialization failed\n");
        goto cleanup_core;
    }
    CPC_SetAudioActive(TRUE);
    if (Cartridge_Insert(rom, (unsigned long)rom_size) != ARNOLD_STATUS_OK) {
        svc->log_printf("GX4000 PAPP: cartridge insert failed\n");
        goto cleanup_core;
    }
    CPC_Reset();
    Cartridge_Autostart();
    svc->log_printf("GX4000 PAPP: running %s\n", rom_path);
    CPCEmulation_Run();

cleanup_core:
    Cartridge_Remove();
    CPCEmulation_Finish();
cleanup:
    if (s_frame) svc->mem_free(s_frame);
    if (rom) svc->mem_free(rom);
    s_frame = NULL;
    return 0;
}
