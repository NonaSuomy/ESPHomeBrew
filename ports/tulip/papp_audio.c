// AMY audio for the Tulip PAPP (replaces AMY's miniaudio driver, which Tulip
// Desktop uses, and Tulip's I2S setup on hardware).
//
// AMY runs in-process with no audio device of its own (AMY_AUDIO_IS_NONE),
// like Tulip's VCV Rack build: this port's audio task pulls 256-frame stereo
// blocks from amy_simple_fill_buffer() and hands them to the loader's
// speaker at 44.1 kHz, paced to real time with a small lead (audio_submit()
// only blocks once the speaker chain is full, which would add its whole
// buffer as latency between a key press and the sound).
#include "papp_port.h"

#include <stdlib.h>
#include <string.h>

#include "amy.h"

extern void tulip_midi_input_hook(uint8_t *data, uint16_t len, uint8_t is_sysex);
extern void tulip_amy_overload_hook(float load);
extern void tulip_amy_sequencer_hook(uint32_t tick_count);

static void *s_task = NULL;
static volatile int s_stop = 0;
static volatile int s_stopped = 1;

#define LEAD_US 60000   // output may run this far ahead of real time
#define TICK_MS 10      // one FreeRTOS tick on this firmware (100 Hz)

static void audio_task(void *arg)
{
    (void)arg;
    int64_t anchor_us = 0;   // when the current run of audio started
    int64_t frames_out = 0;  // frames submitted since anchor_us
    int64_t slept_us = papp_time_us();
    while (!s_stop) {
        const int64_t now = papp_time_us();
        // If rendering cannot keep up, this task never waits: still give the
        // other tasks on this core (ESPHome's loop, idle) a tick now and then.
        if (now - slept_us > 100000) {
            papp_svc->delay_ms(TICK_MS);
            slept_us = papp_time_us();
            continue;
        }
        const int64_t played_us = frames_out * 1000000LL / AMY_SAMPLE_RATE;
        if (frames_out == 0 || now - anchor_us > played_us + 250000) {
            anchor_us = now;  // a new run, or we fell far behind: start over
            frames_out = 0;
        } else {
            const int64_t ahead = played_us - (now - anchor_us);
            if (ahead > LEAD_US) {
                const int ms = (int)((ahead - LEAD_US) / 1000) + 1;
                papp_svc->delay_ms(ms < TICK_MS ? TICK_MS : ms);
                slept_us = papp_time_us();
                continue;
            }
        }
        int16_t *block = amy_simple_fill_buffer();
        frames_out += AMY_BLOCK_SIZE;
        papp_svc->audio_submit(block, AMY_BLOCK_SIZE);
    }
    s_stopped = 1;
    for (;;) {
        papp_svc->delay_ms(1000);  // deleted by papp_audio_stop
    }
}

// Tulip's run_amy() for this platform (tulip/shared/amy_connector.c has the
// ESP32, desktop and VCV variants), called by the MicroPython task at start.
void run_amy(void)
{
    amy_config_t amy_config = amy_default_config();
    amy_config.amy_external_midi_input_hook = tulip_midi_input_hook;
    amy_config.amy_external_overload_hook = tulip_amy_overload_hook;
    amy_config.amy_external_sequencer_hook = tulip_amy_sequencer_hook;
    amy_config.features.default_synths = 0;  // midi.py does this for us
    amy_config.features.audio_in = 0;
    amy_config.features.startup_bleep = 1;   // the milestone's first sound
    amy_config.audio = AMY_AUDIO_IS_NONE;
    amy_config.midi = AMY_MIDI_IS_NONE;
    amy_config.platform.multicore = 0;
    amy_config.platform.multithread = 0;
    amy_start(amy_config);

    if (papp_svc->audio_init == NULL || papp_svc->audio_submit == NULL) {
        papp_svc->log_printf("TULIP: the loader has no audio output\n");
        return;
    }
    papp_svc->audio_init(AMY_SAMPLE_RATE);
    s_stop = 0;
    s_stopped = 0;
    // Core 1, above the display task: a late block is an audible click.
    // AMY's own render tasks use 12-16 KiB; 48 KiB puts the stack in PSRAM
    // (loader: anything over 32 KiB) and leaves internal RAM to the loader.
    if (papp_svc->task_create(audio_task, "tulip_amy", 48 * 1024, NULL, 6, &s_task, 1) != 0) {
        s_task = NULL;
        s_stopped = 1;
        papp_svc->log_printf("TULIP: could not start the audio task\n");
        return;
    }
    papp_svc->log_printf("TULIP: AMY %d Hz, %d-frame blocks\n", AMY_SAMPLE_RATE, AMY_BLOCK_SIZE);
}

void papp_audio_stop(void)
{
    if (s_task != NULL) {
        s_stop = 1;
        for (int i = 0; i < 100 && !s_stopped; i++) {
            papp_svc->delay_ms(10);
        }
        papp_svc->task_delete(s_task);
        s_task = NULL;
    }
}

// ── AMY's platform layer (AMY's i2s.c / libminiaudio-audio.c elsewhere) ─────

void amy_platform_init(void)
{
}

void amy_platform_deinit(void)
{
}

void amy_update_tasks(void)
{
    amy_execute_deltas();
}

int16_t *amy_render_audio(void)
{
    amy_render(0, AMY_OSCS, 0);
    return amy_fill_buffer();
}

size_t amy_i2s_write(const uint8_t *buffer, size_t nbytes)
{
    (void)buffer;
    return nbytes;
}

// MIDI (AMY_HOST_MIDI: this is the host): input is read by poll_midi in
// papp_display.c; output goes to the loader's USB-MIDI device, if it has one.
// AMY's parser keeps SysEx in sysex_buffer, which each platform's run_midi()
// allocates. Without it the first F0 from a keyboard (a tempo or transport
// button sends one) wrote through NULL: a store access fault that rebooted
// the board.
#ifndef MAX_SYSEX_BYTES
#define MAX_SYSEX_BYTES 16384
#endif
extern uint8_t *sysex_buffer;

void papp_midi_ensure_buffer(void)
{
    if (sysex_buffer == NULL) {
        sysex_buffer = (uint8_t *)malloc(MAX_SYSEX_BYTES);
    }
}

void run_midi(void)
{
    papp_midi_ensure_buffer();
}

void stop_midi(void)
{
    free(sysex_buffer);
    sysex_buffer = NULL;
}

void midi_out(uint8_t *bytes, uint16_t len)
{
    if (papp_svc->midi_write != NULL && bytes != NULL && len > 0) {
        papp_svc->midi_write(bytes, len);
    }
}
