// Audio for the OpenLara PAPP (replaces the ESP32-P4 port's I2S/ES8311 code).
//
// OpenLara mixes everything in Sound::fill() (44.1 kHz stereo). The original
// port called it from an audio task; here the game task calls it once per
// frame to top up a ring buffer (papp_openlara.cpp), so all engine code stays
// on one task. This file's task only moves those frames to the loader's
// speaker, paced to real time with a small lead, and fills gaps (a level
// loading) with silence so the speaker chain keeps running.
#include "papp_port.h"

#include <string.h>

enum
{
    RING_FRAMES = 16384,   // ~370 ms
    TARGET_FRAMES = 4096,  // the game keeps ~93 ms queued
    BLOCK_FRAMES = 1024,   // per audio_submit: ~23 ms
};

static int16_t *s_ring = nullptr;          // RING_FRAMES stereo frames
static volatile unsigned s_head = 0;       // frames written (game task)
static volatile unsigned s_tail = 0;       // frames read (audio task)
static volatile bool s_quit = false;
static volatile bool s_done = true;
static void *s_task = nullptr;

// How far the output may run ahead of real time. audio_submit() only blocks
// once the speaker chain is full, which would add its whole buffer as delay.
static const long long LEAD_US = 120000;
static const int TICK_MS = 10; // one FreeRTOS tick on this firmware (100 Hz)

static unsigned queued(void)
{
    return s_head - s_tail;
}

static void audio_task(void *)
{
    static int16_t block[BLOCK_FRAMES * 2];
    long long anchor_us = 0;   // when the current run of audio started
    long long frames_out = 0;  // frames submitted since anchor_us
    while (!s_quit) {
        const long long now = papp_time_us();
        const long long played_us = frames_out * 1000000LL / PAPP_OL_AUDIO_RATE;
        if (frames_out == 0 || now - anchor_us > played_us + 250000) {
            anchor_us = now; // a new run, or we fell far behind: start over
            frames_out = 0;
        } else {
            const long long ahead = played_us - (now - anchor_us);
            if (ahead > LEAD_US) {
                const int ms = static_cast<int>((ahead - LEAD_US) / 1000) + 1;
                papp_svc->delay_ms(ms < TICK_MS ? TICK_MS : ms);
                continue;
            }
            // Not yet a full block and still some output queued downstream:
            // give the game a moment to top the ring up.
            if (queued() < BLOCK_FRAMES && ahead > 40000) {
                papp_svc->delay_ms(TICK_MS);
                continue;
            }
        }
        unsigned n = queued();
        if (n > BLOCK_FRAMES) {
            n = BLOCK_FRAMES;
        }
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        for (unsigned i = 0; i < n; i++) {
            const unsigned at = ((s_tail + i) % RING_FRAMES) * 2;
            block[i * 2] = s_ring[at];
            block[i * 2 + 1] = s_ring[at + 1];
        }
        __atomic_thread_fence(__ATOMIC_RELEASE);
        s_tail += n;
        if (n < BLOCK_FRAMES) {
            memset(block + n * 2, 0, (BLOCK_FRAMES - n) * 2 * sizeof(int16_t)); // underrun: silence
        }
        frames_out += BLOCK_FRAMES;
        papp_svc->audio_submit(block, BLOCK_FRAMES);
    }
    s_done = true;
    for (;;) {
        papp_svc->delay_ms(1000); // deleted by papp_audio_shutdown
    }
}

extern "C" int papp_audio_init(void)
{
    if (papp_svc->audio_init == nullptr || papp_svc->audio_submit == nullptr) {
        return -1;
    }
    s_ring = static_cast<int16_t *>(papp_alloc_raw(RING_FRAMES * 2 * sizeof(int16_t), 0));
    if (s_ring == nullptr) {
        papp_svc->log_printf("OL: no memory for the audio ring\n");
        return -1;
    }
    s_head = s_tail = 0;
    papp_svc->audio_init(PAPP_OL_AUDIO_RATE);
    s_quit = false;
    s_done = false;
    // Core 1 next to ESPHome's loop and the presenter; it mostly sleeps or
    // waits in audio_submit.
    if (papp_svc->task_create(audio_task, "ol_audio", 8 * 1024, nullptr, 6, &s_task, 1) != 0) {
        s_task = nullptr;
        s_done = true;
        papp_svc->mem_free(s_ring);
        s_ring = nullptr;
        papp_svc->log_printf("OL: could not start the audio task\n");
        return -1;
    }
    papp_svc->log_printf("OL: sound %d Hz\n", PAPP_OL_AUDIO_RATE);
    return 0;
}

extern "C" int papp_audio_wanted(void)
{
    if (s_task == nullptr) {
        return 0;
    }
    const unsigned n = queued();
    return n >= TARGET_FRAMES ? 0 : static_cast<int>(TARGET_FRAMES - n);
}

extern "C" void papp_audio_write(const int16_t *stereo, int frames)
{
    if (s_task == nullptr) {
        return;
    }
    const unsigned room = RING_FRAMES - queued();
    const unsigned n = static_cast<unsigned>(frames) < room ? static_cast<unsigned>(frames) : room;
    const unsigned head = s_head;
    for (unsigned i = 0; i < n; i++) {
        const unsigned at = ((head + i) % RING_FRAMES) * 2;
        s_ring[at] = stereo[i * 2];
        s_ring[at + 1] = stereo[i * 2 + 1];
    }
    __atomic_thread_fence(__ATOMIC_RELEASE);
    s_head = head + n;
}

extern "C" void papp_audio_shutdown(void)
{
    if (s_task != nullptr) {
        s_quit = true;
        for (int i = 0; i < 100 && !s_done; i++) {
            papp_svc->delay_ms(10);
        }
        papp_svc->task_delete(s_task);
        s_task = nullptr;
    }
    if (s_ring != nullptr) {
        papp_svc->mem_free(s_ring);
        s_ring = nullptr;
    }
}
