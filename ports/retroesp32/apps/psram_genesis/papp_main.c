#include "runtime.h"
#include "audio_queue.h"
#include "m68k.h"
#include "z80inst.h"
#include "ym2612.h"
#include "gwenesis_bus.h"
#include "gwenesis_io.h"
#include "gwenesis_vdp.h"
#include "gwenesis_sn76489.h"

const app_services_t *_papp_svc;
/* PAL nominally needs 1056 samples; leave room for instruction overshoot. */
#define GEN_AUDIO_CAP 1152
unsigned char *M68K_RAM, *ZRAM;
int16_t *gwenesis_sn76489_buffer, *gwenesis_ym2612_buffer;
int sn76489_index, sn76489_clock, ym2612_index, ym2612_clock;
int32_t *lfo_pm_table;
signed int *tl_tab;
int scan_line, frame_counter;
extern unsigned char *ROM_DATA, *VRAM, *SAT_CACHE, *gwenesis_vdp_regs;
extern unsigned short *CRAM, *CRAM565, *VSRAM, *fifo;
extern uint8_t *render_buffer, *sprite_buffer, *OPNREGS;
extern unsigned int *sin_tab;
extern m68ki_cpu_core *m68k;
extern YM2612 *ym2612;
extern int zclk, hint_pending, screen_width, screen_height;
extern unsigned short gwenesis_vdp_status;
extern void *heap_caps_malloc(size_t size, uint32_t caps);
static jmp_buf exit_env;
void app_return_to_launcher(void) { longjmp(exit_env,1); }
void gwenesis_io_get_buttons(void) {}

static void *allocate(size_t bytes, int hot)
{
    void *p = hot ? heap_caps_malloc(bytes,PAPP_MEM_CAP_INTERNAL) : NULL;
    if (!p) p = malloc(bytes);
    if (!p) app_return_to_launcher();
    memset(p,0,bytes);
    return p;
}

static void allocate_core(void)
{
    m68k = allocate(sizeof(*m68k),1);
    M68K_RAM = allocate(MAX_RAM_SIZE,1);
    ZRAM = allocate(MAX_Z80_RAM_SIZE,1);
    VRAM = allocate(VRAM_MAX_SIZE,1);
    CRAM = allocate(CRAM_MAX_SIZE*sizeof(*CRAM),1);
    CRAM565 = allocate(CRAM_MAX_SIZE*4*sizeof(*CRAM565),1);
    VSRAM = allocate(VSRAM_MAX_SIZE*sizeof(*VSRAM),1);
    SAT_CACHE = allocate(SAT_CACHE_MAX_SIZE,1);
    gwenesis_vdp_regs = allocate(REG_SIZE,1);
    fifo = allocate(FIFO_SIZE*sizeof(*fifo),1);
    render_buffer = allocate(SCREEN_WIDTH+PIX_OVERFLOW*2,1);
    sprite_buffer = allocate(SCREEN_WIDTH+PIX_OVERFLOW*2,1);
    ym2612 = allocate(sizeof(*ym2612),1);
    OPNREGS = allocate(512,1);
    sin_tab = allocate(SIN_LEN*sizeof(*sin_tab),1);
    tl_tab = allocate(13*2*256*sizeof(*tl_tab),1);
    lfo_pm_table = allocate(128*8*32*sizeof(*lfo_pm_table),0);
    gwenesis_sn76489_buffer = allocate(GEN_AUDIO_CAP*sizeof(int16_t),1);
    gwenesis_ym2612_buffer = allocate(GEN_AUDIO_CAP*sizeof(int16_t),1);
}

__attribute__((section(".text.entry")))
int app_entry(const app_services_t *svc)
{
    if (!svc || svc->abi_version != PAPP_ABI_VERSION) return -1;
    _papp_svc = svc;
    char path[512];
    if (emu_rom_path("genesis",".md|.gen|.bin|.smd|",path,sizeof(path))) {
        svc->log_printf("Genesis: set ROM path in /sd/roms/papp/genesis.rom\n");
        return -1;
    }
    volatile int result = -1;
    if (!setjmp(exit_env)) {
        allocate_core();
        FILE *f = fopen(path,"rb");
        if (!f) app_return_to_launcher();
        fseek(f,0,SEEK_END);
        long size = ftell(f);
        fseek(f,0,SEEK_SET);
        if (size < 512 || size > MAX_ROM_SIZE) app_return_to_launcher();
        ROM_DATA = allocate(size,0);
        if (fread(ROM_DATA,1,size,f) != (size_t)size) app_return_to_launcher();
        fclose(f);
        uint8_t *indexed = allocate(320*240,0);
        uint16_t *pixels = allocate(320*240*2,0);
        int16_t *audio = allocate(GEN_AUDIO_CAP*2*sizeof(int16_t),0);
        load_cartridge(ROM_DATA,size);
        power_on();
        reset_emulation();
        gwenesis_vdp_set_buffer(indexed);
        int rate = (REG1_PAL ? GWENESIS_AUDIO_FREQ_PAL:GWENESIS_AUDIO_FREQ_NTSC)/2;
        emu_audio_start(rate);
        svc->log_printf("Genesis PAPP: running %s, audio=%d Hz queued=%d\n",path,rate,emu_audio.task != NULL);
        emu_clock clock = {svc->get_time_us(),0,REG1_PAL ? 50:60};
        int64_t started = clock.deadline;
        unsigned skipped=0, changes=0, last_hash=0, drawn=0;
        int peak=0;
        for (;;) {
            papp_gamepad_state_t pad;
            svc->input_gamepad_read(&pad);
#ifdef PAPP_TEST_FRAMES
            memset(&pad,0,sizeof(pad));
            pad.values[PAPP_INPUT_START] = frame_counter%180 < 5;
            pad.values[PAPP_INPUT_A] = frame_counter>=300 && frame_counter%120<5;
            pad.values[PAPP_INPUT_RIGHT] = frame_counter>=600;
#endif
            /* The shared launcher aliases X to MENU for two-button cores.
             * Genesis needs X as its third action button. Keep touch MENU
             * and dedicated L3 available without closing on that alias. */
            if ((pad.values[PAPP_INPUT_MENU] && !pad.values[PAPP_INPUT_X]) ||
                (svc->input_l3_read && svc->input_l3_read())) break;
            for (int i=0; i<8; ++i) gwenesis_io_pad_release_button(0,i);
            const int buttons[] = {PAD_UP,PAD_RIGHT,PAD_DOWN,PAD_LEFT,-1,PAD_S,PAD_B,PAD_C};
            for (int i=0; i<8; ++i) if (buttons[i]>=0 && pad.values[i]) gwenesis_io_pad_press_button(0,buttons[i]);
            if (pad.values[PAPP_INPUT_X] || pad.values[PAPP_INPUT_Y]) gwenesis_io_pad_press_button(0,PAD_A);
            int draw = !(svc->get_time_us() > clock.deadline+1000000/clock.hz && skipped<2);
            skipped = draw ? 0:skipped+1;
            int lines = REG1_PAL ? LINES_PER_FRAME_PAL:LINES_PER_FRAME_NTSC;
            screen_width = REG12_MODE_H40 ? 320:256;
            screen_height = REG1_PAL ? 240:224;
            int hint_counter = gwenesis_vdp_regs[10];
            gwenesis_vdp_render_config();
            int system_clock = 0;
            zclk = 0;
            sn76489_clock=sn76489_index=ym2612_clock=ym2612_index=0;
            for (scan_line=0; scan_line<lines;) {
                system_clock += VDP_CYCLES_PER_LINE;
                m68k_run(system_clock);
                z80_run(system_clock);
                if (draw && scan_line<screen_height) gwenesis_vdp_render_line(scan_line);
                if (!scan_line || scan_line>screen_height) hint_counter=REG10_LINE_COUNTER;
                if (--hint_counter<0) {
                    if (REG0_LINE_INTERRUPT && scan_line<=screen_height) {
                        hint_pending=1;
                        if (!(gwenesis_vdp_status & STATUS_VIRQPENDING)) m68k_update_irq(4);
                    }
                    hint_counter=REG10_LINE_COUNTER;
                }
                if (++scan_line==screen_height) {
                    if (REG1_VBLANK_INTERRUPT) {
                        gwenesis_vdp_status |= STATUS_VIRQPENDING;
                        m68k_set_irq(6);
                    }
                    z80_irq_line(1);
                }
                if (scan_line==screen_height+1) z80_irq_line(0);
            }
            m68k->cycles -= system_clock;
            gwenesis_SN76489_run(system_clock);
            ym2612_run(system_clock);
            int count = sn76489_index > ym2612_index ? sn76489_index:ym2612_index;
            if (count<0 || count>GEN_AUDIO_CAP) app_return_to_launcher();
            int out_count=count/2;
            for (int i=0; i<out_count; ++i) {
                int sample=0;
                for (int j=2*i; j<2*i+2; ++j) {
                    if (j<sn76489_index) sample+=gwenesis_sn76489_buffer[j];
                    if (j<ym2612_index) sample+=gwenesis_ym2612_buffer[j];
                }
                sample=(sample/2)*2/3;
                if (sample>32767) sample=32767;
                if (sample<-32768) sample=-32768;
                audio[2*i]=audio[2*i+1]=sample;
            }
            if (draw) {
                ++drawn;
                for (int y=0; y<screen_height; ++y)
                    for (int x=0; x<screen_width; ++x)
                        pixels[y*screen_width+x]=CRAM565[indexed[y*320+x]];
                svc->display_write_frame_custom(pixels,screen_width,screen_height,2.0f,false);
            }
            if (out_count) emu_audio_submit(audio,out_count);
            ++frame_counter;
#ifdef PAPP_TEST_FRAMES
            unsigned hash=2166136261u;
            for (int i=0; i<screen_width*screen_height; i+=17) hash=(hash^pixels[i])*16777619u;
            if (frame_counter>1 && hash!=last_hash) ++changes;
            last_hash=hash;
            for (int i=0; i<2*out_count; ++i) { int v=audio[i]; if(v<0)v=-v; if(v>peak)peak=v; }
            if (frame_counter>=PAPP_TEST_FRAMES) {
                svc->log_printf("Genesis test: frames=%d changed=%u audio_peak=%d elapsed_ms=%ld\n",frame_counter,changes,peak,(long)((svc->get_time_us()-started)/1000));
                svc->log_printf("Genesis drawn_frames=%u\n",drawn);
                break;
            }
#endif
            emu_pace(&clock);
        }
        result=0;
    }
    emu_audio_finish();
    papp_close_streams(); papp_cleanup_fds(); papp_cleanup_heap();
    svc->display_clear(0); svc->display_flush();
    return result;
}
