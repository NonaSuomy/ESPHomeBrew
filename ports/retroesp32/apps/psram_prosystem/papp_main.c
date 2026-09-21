#include "runtime.h"
#include "Cartridge.h"
#include "Database.h"
#include "Maria.h"
#include "Palette.h"
#include "Pokey.h"
#include "ProSystem.h"
#include "Tia.h"
#include "Sally.h"

const app_services_t *_papp_svc;
bool RenderFlag = true;
static jmp_buf exit_env;
void app_return_to_launcher(void) { longjmp(exit_env,1); }

__attribute__((section(".text.entry")))
int app_entry(const app_services_t *svc)
{
    if (!svc || svc->abi_version != PAPP_ABI_VERSION) return -1;
    _papp_svc = svc;
    char path[512];
    if (emu_rom_path("prosystem",".a78|.bin|",path,sizeof(path))) {
        svc->log_printf("A7800: set ROM path in /sd/roms/papp/prosystem.rom\n");
        return -1;
    }
    volatile int result = -1;
    if (!setjmp(exit_env)) {
        FILE *f = fopen(path,"rb");
        if (!f) app_return_to_launcher();
        fseek(f,0,SEEK_END);
        long size = ftell(f);
        fseek(f,0,SEEK_SET);
        if (size <= 128 || size > 4*1024*1024) { fclose(f); app_return_to_launcher(); }
        uint8_t *rom = malloc(size);
        if (!rom) { fclose(f); app_return_to_launcher(); }
        size_t got = fread(rom,1,size,f);
        fclose(f);
        if (got != (size_t)size || !cartridge_Load(rom,size)) app_return_to_launcher();
        free(rom);
        maria_surface = calloc(320,292);
        uint16_t *pixels = calloc(320*240,2);
        int16_t *audio = calloc(1024,2*sizeof(int16_t));
        if (!maria_surface || !pixels || !audio) app_return_to_launcher();
        database_Load(cartridge_digest);
        svc->log_printf("A7800 cartridge: %s type=%u bytes=%ld\n",cartridge_digest,cartridge_type,size);
        prosystem_Reset();
        if (prosystem_frequency != 50 && prosystem_frequency != 60) app_return_to_launcher();
        uint16_t palette[256];
        for (int i=0; i<256; ++i) palette[i] = ((palette_data[3*i]&248)<<8) |
            ((palette_data[3*i+1]&252)<<3) | (palette_data[3*i+2]>>3);
        svc->audio_init(32000);
        svc->log_printf("Atari 7800 PAPP: running %s (%u Hz)\n",path,prosystem_frequency);
        emu_clock clock = {svc->get_time_us(),0,prosystem_frequency};
        int64_t started = clock.deadline;
        unsigned skipped = 0;
        unsigned audio_phase = 0;
        unsigned frames = 0, changes = 0, last_hash = 0;
        int peak = 0, minimum = 32767, maximum = -32768;
        int32_t previous_sample = 0, filtered = 0;
        int raw_minimum = 32767, raw_maximum = -32768;
        uint8_t keys[17] = {0}; keys[15] = 1;
        for (;;) {
            papp_gamepad_state_t pad;
            svc->input_gamepad_read(&pad);
#ifdef PAPP_TEST_FRAMES
            memset(&pad,0,sizeof(pad));
            pad.values[PAPP_INPUT_START] = frames >= 60 && frames < 65;
#endif
            if (emu_quit(&pad)) break;
            const int mapping[] = {PAPP_INPUT_RIGHT,PAPP_INPUT_LEFT,PAPP_INPUT_DOWN,PAPP_INPUT_UP,PAPP_INPUT_B,PAPP_INPUT_A};
            for (int i=0; i<6; ++i) keys[i] = !!pad.values[mapping[i]];
            keys[13] = !!pad.values[PAPP_INPUT_SELECT];
            keys[14] = !!pad.values[PAPP_INPUT_START];
            RenderFlag = !(svc->get_time_us() > clock.deadline + 1000000/clock.hz && skipped < 2);
            skipped = RenderFlag ? 0 : skipped+1;
            prosystem_ExecuteFrame(keys);
            int top = maria_visibleArea.top;
            if (maria_visibleArea.bottom-top < 271) top -= 24;
            if (top < 0) top = 0;
            if (top > 52) top = 52;
            if (RenderFlag) {
                for (int i=0; i<320*240; ++i) pixels[i] = palette[maria_surface[top*320+i]];
                svc->display_write_frame_custom(pixels,320,240,2.0f,false);
            }
            audio_phase += 32000;
            unsigned count = audio_phase / prosystem_frequency;
            audio_phase %= prosystem_frequency;
            unsigned source_count = prosystem_scanlines*2;
            for (unsigned i=0; i<count; ++i) {
                unsigned j = i*source_count/count;
                int sample = (int)tia_buffer[j]-128;
                if (cartridge_pokey) sample = (sample+(int)pokey_buffer[j]-128)/2;
                /* Remove the DC bias of the unsigned TIA/Pokey mixer. */
                int32_t value = sample*256;
#ifdef PAPP_TEST_FRAMES
                if (value < raw_minimum) raw_minimum = value;
                if (value > raw_maximum) raw_maximum = value;
#endif
                filtered = value - previous_sample + (filtered*32604)/32768;
                previous_sample = value;
                int32_t clipped = filtered;
                if (clipped > 32767) clipped = 32767;
                if (clipped < -32768) clipped = -32768;
                audio[2*i] = audio[2*i+1] = clipped;
            }
            svc->audio_submit(audio,count);
#ifdef PAPP_TEST_FRAMES
            {
                unsigned hash = 2166136261u;
                for (int i=0; i<320*240; i+=17) hash = (hash ^ pixels[i])*16777619u;
                if (frames && hash != last_hash) ++changes;
                last_hash = hash;
                for (unsigned i=0; i<2*count; ++i) {
                    int v=audio[i];
                    if (v<minimum) minimum=v; if (v>maximum) maximum=v;
                    if (v<0) v=-v; if (v>peak) peak=v;
                }
            }
            if (++frames >= PAPP_TEST_FRAMES) {
                svc->log_printf("A7800 test: frames=%u changed=%u audio_peak=%d range=%d..%d\n",frames,changes,peak,minimum,maximum);
                svc->log_printf("A7800 elapsed_ms=%ld\n",(long)((svc->get_time_us()-started)/1000));
                svc->log_printf("A7800 raw_audio_range=%d..%d\n",raw_minimum,raw_maximum);
                break;
            }
#endif
            emu_pace(&clock);
        }
        prosystem_Close();
        result = 0;
    }
    papp_close_streams();
    papp_cleanup_fds();
    papp_cleanup_heap();
    svc->display_clear(0); svc->display_flush();
    return result;
}
