/* Synchronous SID output adapter for a PSRAM-loaded PAPP. */

#include "psram_app.h"
#include "VIC.h"
#include <string.h>

#define PAPP_AUDIO_FRAMES 256

static int16_t papp_sid_mono[PAPP_AUDIO_FRAMES];
static int16_t papp_sid_stereo[PAPP_AUDIO_FRAMES * 2];
static int papp_sid_pending;
static bool papp_sid_paused;
static int papp_sid_divisor;

void DigitalRenderer::init_sound(void)
{
    papp_sid_pending = 0;
    papp_sid_divisor = 0;
    papp_sid_paused = false;
    ready = true;
    if (_papp_svc && _papp_svc->audio_init)
        _papp_svc->audio_init((int)SAMPLE_FREQ);
}

void DigitalRenderer::EmulateLine(void)
{
    if (!ready || papp_sid_paused) return;

    papp_sid_divisor += SAMPLE_FREQ;
    while (papp_sid_divisor >= 0) {
        papp_sid_divisor -= TOTAL_RASTERS * SCREEN_FREQ;
        ++papp_sid_pending;
    }

    sample_buf[sample_in_ptr] = volume;
    sample_in_ptr = (sample_in_ptr + 1) % SAMPLE_BUF_SIZE;

    if (papp_sid_pending >= PAPP_AUDIO_FRAMES) {
        papp_sid_pending -= PAPP_AUDIO_FRAMES;
        calc_buffer(papp_sid_mono, PAPP_AUDIO_FRAMES * 2);

        /* Frodo's original renderer includes the SID's volume-dependent DC
         * offset in every sample.  That is useful for an analogue SID model,
         * but it becomes an audible constant buzz when the voices are idle
         * and the samples are sent to a digital I2S amplifier.  Keep the
         * rendered waveform for active voices, but emit true digital silence
         * while the SID has no envelope currently producing sound. */
        int16_t block_min = papp_sid_mono[0];
        int16_t block_max = papp_sid_mono[0];
        for (int i = 1; i < PAPP_AUDIO_FRAMES; ++i) {
            if (papp_sid_mono[i] < block_min)
                block_min = papp_sid_mono[i];
            if (papp_sid_mono[i] > block_max)
                block_max = papp_sid_mono[i];
        }
        // A silent SID block is commonly a constant DC value (for example
        // 8192), not literal zero, so checking only the absolute peak is not
        // sufficient.  A small threshold avoids waking the speaker for a
        // negligible filter residue while preserving quiet real waveforms.
        const bool has_waveform = (static_cast<int>(block_max) - static_cast<int>(block_min)) > 8;
        if (!has_waveform)
            memset(papp_sid_mono, 0, sizeof(papp_sid_mono));

        for (int i = 0; i < PAPP_AUDIO_FRAMES; ++i) {
            papp_sid_stereo[i * 2] = papp_sid_mono[i];
            papp_sid_stereo[i * 2 + 1] = papp_sid_mono[i];
        }
        // Do not keep the speaker pipeline alive with empty startup/menu
        // blocks.  The ESPHome speaker can then remain stopped until the
        // first real SID voice starts, avoiding an initial ring-buffer write
        // while its resampler task is still coming up.
        if (has_waveform && _papp_svc && _papp_svc->audio_submit)
            _papp_svc->audio_submit(papp_sid_stereo, PAPP_AUDIO_FRAMES);
    }
}

void DigitalRenderer::changeVolumeLevel(void) {}
void DigitalRenderer::Pause(void) { papp_sid_paused = true; }
void DigitalRenderer::Resume(void) { papp_sid_paused = false; }
DigitalRenderer::~DigitalRenderer(void) {}
