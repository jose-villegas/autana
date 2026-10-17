/* Blocking PCM output, used from a worker or console task. */
#include "audio/audio_out.h"

#include <limits.h>

#include "audio/audio_codec.h"
#include "audio/audio_out_device.h"
#include "audio/audio_tone.h"
#include "board/board.h"
#include "core/timing.h"

#define AUDIO_OUT_AMP_STARTUP_MS 250
#define AUDIO_OUT_CODEC_VOLUME   70
#define AUDIO_OUT_DRAIN_MS                                                                                             \
    ((AUDIO_CODEC_DMA_BLOCKS * AUDIO_CODEC_BLOCK_SAMPLES * MATH_MS_PER_S + AUDIO_OUT_SAMPLE_RATE_HZ - 1)               \
     / AUDIO_OUT_SAMPLE_RATE_HZ)
static esp_codec_dev_handle_t output;

bool
audio_out_amp(bool enabled) {
    audio_codec_assert_not_frame_task();
    if (!audio_codec_check("amplifier", board_audio_amp_enable(enabled))) {
        return false;
    }
    if (enabled) {
        /* The amplifier passes sound 150-190 ms after enable; allow startup margin. */
        timing_sleep_ms(AUDIO_OUT_AMP_STARTUP_MS);
    }
    return true;
}

bool
audio_out_open(void) {
    if (output) {
        return true;
    }
    output = audio_codec_device_open(ESP_CODEC_DEV_TYPE_OUT);
    if (!output) {
        return false;
    }
    if (!audio_codec_check("volume", esp_codec_dev_set_out_vol(output, AUDIO_OUT_CODEC_VOLUME))) {
        audio_out_close();
        return false;
    }
    return true;
}

bool
audio_out_write(const int16_t* samples, size_t count) {
    audio_codec_assert_not_frame_task();
    if (!output || !samples || count > INT_MAX / sizeof *samples) {
        return false;
    }
    return audio_codec_check("write", esp_codec_dev_write(output, (void*)samples, (int)(count * sizeof *samples)));
}

bool
audio_out_tone(uint16_t hz, uint16_t ms, uint8_t volume_pct) {
    if (!output || !audio_out_args_valid(hz, ms, volume_pct)) {
        return false;
    }
    audio_tone_t tone;
    audio_tone_init(&tone, hz);
    int16_t samples[AUDIO_CODEC_BLOCK_SAMPLES];
    size_t remaining = audio_out_samples(ms);
    bool ok = audio_out_amp(true);
    while (ok && remaining) {
        const size_t n = remaining < AUDIO_CODEC_BLOCK_SAMPLES ? remaining : AUDIO_CODEC_BLOCK_SAMPLES;
        audio_tone_fill(&tone, samples, n, volume_pct);
        ok = audio_out_write(samples, n);
        remaining -= n;
    }
    /* Writes queue DMA; keep the amplifier up until the final block reaches the DAC. */
    if (ok) {
        timing_sleep_ms(AUDIO_OUT_DRAIN_MS);
    }
    const bool off = audio_out_amp(false);
    return ok && off;
}

void
audio_out_close(void) {
    audio_codec_device_close(output);
    output = NULL;
}
