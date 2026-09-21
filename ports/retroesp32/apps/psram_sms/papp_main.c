#define PAPP_APP_SIDE 1
#include "psram_app.h"
#include "runtime.h"
#include "shared.h"
#include <setjmp.h>
#include <strings.h>

const app_services_t *_papp_svc;
uint8 *ESP32_PSRAM;
static jmp_buf exit_env;
static char rom_path[512];
extern void papp_cleanup_heap(void);
extern void papp_cleanup_fds(void);
extern void papp_close_streams(void);
void app_return_to_launcher(void) { longjmp(exit_env, 1); }
/* SRAM support is deliberately disabled until save compatibility is tested.
 * Do not overwrite saves made by the native emulator. */
void system_manage_sram(uint8 *sram, int slot, int mode) {
    (void)sram; (void)slot; (void)mode;
}

__attribute__((section(".text.entry")))
int app_entry(const app_services_t *svc)
{
    if (!svc || svc->abi_version != PAPP_ABI_VERSION) return -1;
    _papp_svc = svc;
    /* ZIP archives are unpacked into the shared cache before the legacy
     * path-based SMS/GG loader opens the ROM. */
    if (emu_rom_path("sms", ".sms|.gg|", rom_path, sizeof(rom_path))) {
        svc->log_printf("SMS: select a .sms/.gg ROM or ZIP archive\n");
        return -1;
    }
    volatile int result = -1;
    if (!setjmp(exit_env)) {
        ESP32_PSRAM = calloc(1,0x200000);
        bitmap.data = calloc(256,256);
        uint16_t *pixels = calloc(256*256,2);
        int16_t *audio = calloc(2048,2*sizeof(int16_t));
        if (!ESP32_PSRAM || !bitmap.data || !pixels || !audio) app_return_to_launcher();
        set_option_defaults();
        option.sndrate = 32000;
        option.overscan = 0;
        option.extra_gg = 0;
        if (!load_rom(rom_path)) app_return_to_launcher();
        sms.use_fm = 0;
        bitmap.width = 256;
        bitmap.height = 192;
        bitmap.pitch = 256;
        system_init2();
        system_reset();
        svc->audio_init(32000);
        svc->log_printf("SMS/GG PAPP: running %s\n",rom_path);
        int64_t deadline = svc->get_time_us();
        int64_t started = deadline;
        unsigned skipped = 0;
        const int hz = sms.display == DISPLAY_PAL ? 50 : 60;
        unsigned phase = 0;
        unsigned frames = 0, changes = 0, last_hash = 0;
        int peak = 0;
        for (;;) {
            papp_gamepad_state_t pad;
            svc->input_gamepad_read(&pad);
#ifdef PAPP_TEST_FRAMES
            memset(&pad,0,sizeof(pad));
            pad.values[PAPP_INPUT_START] = (frames >= 60 && frames < 65) || (frames >= 180 && frames < 185);
#endif
            if (pad.values[PAPP_INPUT_MENU] || (svc->input_l3_read && svc->input_l3_read())) break;
            input.pad[0] = (pad.values[PAPP_INPUT_UP] ? INPUT_UP:0) |
                (pad.values[PAPP_INPUT_DOWN] ? INPUT_DOWN:0) |
                (pad.values[PAPP_INPUT_LEFT] ? INPUT_LEFT:0) |
                (pad.values[PAPP_INPUT_RIGHT] ? INPUT_RIGHT:0) |
                (pad.values[PAPP_INPUT_A] ? INPUT_BUTTON2:0) |
                (pad.values[PAPP_INPUT_B] ? INPUT_BUTTON1:0);
            input.system = (pad.values[PAPP_INPUT_START] ? (IS_GG ? INPUT_START:INPUT_PAUSE):0) |
                (pad.values[PAPP_INPUT_SELECT] ? INPUT_PAUSE:0);
            int skip = svc->get_time_us() > deadline + 1000000/hz && skipped < 2;
            skipped = skip ? skipped+1 : 0;
            system_frame(skip);
            uint16_t palette[32];
            render_copy_palette(palette);
            int w = IS_GG ? 160 : 256, h = IS_GG ? 144 : bitmap.viewport.h;
            if (h <= 0 || h > 240) h = 192;
            /* render_line already removes the Game Gear vertical border. */
            int x0 = IS_GG ? 48 : 0, y0 = 0;
            if (!skip) for (int y=0; y<h; ++y)
                for (int x=0; x<w; ++x)
                    pixels[y*w+x] = palette[bitmap.data[(y+y0)*256+x+x0] & PIXEL_MASK];
            if (!skip) svc->display_write_frame_custom(pixels,w,h,IS_GG ? 3.0f:2.0f,false);
            if (snd.sample_count < 0 || snd.sample_count > 2048) app_return_to_launcher();
            for (int i=0; i<snd.sample_count; ++i) {
                audio[2*i] = snd.output[0][i]; audio[2*i+1] = snd.output[1][i];
            }
            svc->audio_submit(audio,snd.sample_count);
#ifdef PAPP_TEST_FRAMES
            {
                unsigned hash = 2166136261u;
                for (int i=0; i<w*h; i+=17) hash = (hash ^ pixels[i])*16777619u;
                if (frames && hash != last_hash) ++changes;
                last_hash = hash;
                for (int i=0; i<2*snd.sample_count; ++i) {
                    int v = audio[i]; if (v<0) v=-v; if (v>peak) peak=v;
                }
            }
            if (++frames >= PAPP_TEST_FRAMES) {
                svc->log_printf("SMS test: frames=%u changed=%u audio_peak=%d elapsed_ms=%ld\n",frames,changes,peak,(long)((svc->get_time_us()-started)/1000));
                break;
            }
#endif
            phase += 1000000;
            deadline += phase / hz;
            phase %= hz;
            int64_t now = svc->get_time_us();
            if (deadline > now) svc->delay_ms((int)((deadline-now)/1000));
            else if (now-deadline > 50000) deadline = now;
        }
        system_shutdown();
        result = 0;
    }
    papp_close_streams();
    papp_cleanup_fds();
    papp_cleanup_heap();
    svc->display_clear(0);
    svc->display_flush();
    return result;
}
