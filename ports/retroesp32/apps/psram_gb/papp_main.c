#define PAPP_APP_SIDE 1
#include "psram_app.h"
#include "runtime.h"
#include <setjmp.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "gnuboy.h"
#include "loader.h"
#include "cpu.h"
#include "hw.h"
#include "lcd.h"
#include "fb.h"
#include "pcm.h"
#include "sound.h"
#include "rtc.h"
#include "regs.h"
#include "mem.h"

const app_services_t *_papp_svc;
const char *SD_BASE_PATH = "/sd";
struct fb fb;
struct pcm pcm;
uint16_t *displayBuffer[2];
int frame;
static jmp_buf exit_env;
static char rom_path[512];
extern FILE *RomFile;
extern void papp_cleanup_heap(void);
extern void papp_cleanup_fds(void);
extern void papp_close_streams(void);

char *odroid_settings_RomFilePath_get(void) { return rom_path; }
void app_return_to_launcher(void) { longjmp(exit_env, 1); }
void odroid_display_show_sderr(int error) {
    _papp_svc->log_printf("GB: ROM read error %d\n", error);
}
int pcm_submit(void) { return 1; }

__attribute__((section(".text.entry")))
int app_entry(const app_services_t *svc)
{
    if (!svc || svc->abi_version != PAPP_ABI_VERSION) return -1;
    _papp_svc = svc;
    /* The shared helper also turns a .zip containing a .gb/.gbc entry into
     * a cached raw image, so the legacy Gnuboy loader can keep using a path. */
    if (emu_rom_path("gb", ".gb|.gbc|", rom_path, sizeof(rom_path))) {
        svc->log_printf("GB: select a .gb/.gbc ROM or ZIP archive\n");
        return -1;
    }
    void *probe = svc->file_open(rom_path, "rb");
    if (!probe) {
        svc->log_printf("GB: select a .gb/.gbc ROM or provide %s\n", rom_path);
        return -1;
    }
    unsigned char header[0x150];
    size_t got = svc->file_read(header, 1, sizeof(header), probe);
    svc->file_seek(probe, 0, SEEK_END);
    long rom_bytes = svc->file_tell(probe);
    svc->file_close(probe);
    /* This core reserves 4 MiB for lazy ROM banks. Reject unsupported or
     * truncated images before the legacy loader can index outside it. */
    if (got != sizeof(header) || header[0x148] > 7 || header[0x149] > 5 ||
        rom_bytes < (32768L << header[0x148]) || rom_bytes > 4194304L) {
        svc->log_printf("GB: invalid/truncated or unsupported (>4 MiB) ROM\n");
        return -1;
    }
    volatile int result = -1;
    if (!setjmp(exit_env)) {
        fb = (struct fb){ .w=160, .h=144, .pelsize=2, .pitch=320,
            .enabled=1, .dirty=1 };
        fb.ptr = calloc(160 * 144, 2);
        pcm = (struct pcm){ .hz=32000, .stereo=1, .len=6402 };
        pcm.buf = calloc(pcm.len, sizeof(*pcm.buf));
        if (!fb.ptr || !pcm.buf) app_return_to_launcher();
        displayBuffer[0] = displayBuffer[1] = (uint16_t *)fb.ptr;
        loader_init(NULL);
        /* Read all banks before play, avoiding mid-frame SD stalls. */
        if (fseek(RomFile,0,SEEK_SET) || fread(rom.bank[0],1,rom.length,RomFile) != (size_t)rom.length)
            app_return_to_launcher();
        fclose(RomFile);
        RomFile = NULL;
        if (sram_load() < 0 && mbc.batt) {
            svc->log_printf("GB: refusing to overwrite an incomplete SRAM save\n");
            app_return_to_launcher();
        }
        emu_reset();
        sound_reset();
        lcd_begin();
        svc->audio_init(pcm.hz);
        svc->log_printf("GB PAPP: running %s\n", rom_path);
        int64_t deadline = svc->get_time_us();
        int64_t started = deadline;
        unsigned skipped = 0;
        unsigned remainder = 0;
        int peak = 0;
        unsigned last_hash = 0, changes = 0;
        for (;;) {
            papp_gamepad_state_t pad;
            svc->input_gamepad_read(&pad);
#ifdef PAPP_TEST_FRAMES
            memset(&pad,0,sizeof(pad));
            pad.values[PAPP_INPUT_START] = (frame >= 60 && frame < 65) || (frame >= 180 && frame < 185);
            if (frame >= 300) pad.values[PAPP_INPUT_START] = frame % 180 < 5;
            pad.values[PAPP_INPUT_A] = frame >= 300 && frame % 120 < 5;
#endif
            if (pad.values[PAPP_INPUT_MENU] || (svc->input_l3_read && svc->input_l3_read())) break;
            const int buttons[] = {PAD_UP,PAD_RIGHT,PAD_DOWN,PAD_LEFT,PAD_SELECT,PAD_START,PAD_A,PAD_B};
            for (int i=0; i<8; ++i) pad_set(buttons[i], pad.values[i]);
            fb.enabled = !(svc->get_time_us() > deadline + 16743 && skipped < 2);
            skipped = fb.enabled ? 0 : skipped+1;
            cpu_emulate(2280);
            while (R_LY > 0 && R_LY < 144) emu_step();
            if (fb.enabled) svc->display_write_frame_custom((uint16_t *)fb.ptr,160,144,3.0f,false);
            rtc_tick();
            sound_mix();
#ifdef PAPP_TEST_FRAMES
            {
                unsigned hash = 2166136261u;
                for (int i=0; i<160*144*2; i+=31) hash = (hash ^ fb.ptr[i])*16777619u;
                if (frame && hash != last_hash) ++changes;
                last_hash = hash;
                for (int i=0; i<pcm.pos; ++i) {
                    int v = pcm.buf[i]; if (v < 0) v = -v;
                    if (v > peak) peak = v;
                }
            }
#endif
            if (pcm.pos) svc->audio_submit(pcm.buf, pcm.pos / 2);
            pcm.pos = 0;
            if (!(R_LCDC & 0x80)) cpu_emulate(32832);
            while (R_LY > 0) emu_step();
            ++frame;
#ifdef PAPP_TEST_FRAMES
            if (frame >= PAPP_TEST_FRAMES) {
                svc->log_printf("GB test: frames=%d changed=%u audio_peak=%d\n",frame,changes,peak);
                svc->log_printf("GB APU: NR50=%02x NR51=%02x NR52=%02x PC=%04x elapsed_ms=%ld\n",R_NR50,R_NR51,R_NR52,cpu.pc.w[0],(long)((svc->get_time_us()-started)/1000));
                extern unsigned papp_apu_writes, papp_apu_triggers;
                svc->log_printf("GB APU writes=%u triggers=%u\n",papp_apu_writes,papp_apu_triggers);
                break;
            }
#endif
            /* Hardware frame period: 70224 cycles at 4194304 Hz. */
            remainder += 11572;
            deadline += 16742 + remainder / 16384;
            remainder %= 16384;
            int64_t now = svc->get_time_us();
            if (deadline > now) svc->delay_ms((int)((deadline-now)/1000));
            else if (now-deadline > 50000) deadline = now;
        }
        loader_unload();
        result = 0;
    }
    if (RomFile) { fclose(RomFile); RomFile = NULL; }
    papp_close_streams();
    papp_cleanup_fds();
    papp_cleanup_heap();
    svc->display_clear(0);
    svc->display_flush();
    return result;
}
