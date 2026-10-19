/* Capture and detection do not own playback. */
#include "audio/audio_loopback.h"

#include <math.h>
#include "audio/audio_codec.h"

#include "math/scalar/math_const.h"
#ifdef ESP_PLATFORM
#include "audio/audio_in.h"
#endif
#include "math/signal/goertzel.h"

void
audio_loopback_frequency_init(audio_loopback_frequency_t* state, uint16_t hz) {
    *state = (audio_loopback_frequency_t){.nominal_hz = (float)hz};
    goertzel_init(&state->bin, (float)hz, (float)AUDIO_OUT_SAMPLE_RATE_HZ);
}

void
audio_loopback_frequency_feed(audio_loopback_frequency_t* state, const int16_t* samples, size_t count) {
    while (count) {
        const size_t space = AUDIO_LOOPBACK_WINDOW_SAMPLES - state->bin.samples;
        const size_t n = count < space ? count : space;
        goertzel_feed_s(&state->bin, samples, n);
        samples += n;
        count -= n;
        if (state->bin.samples == AUDIO_LOOPBACK_WINDOW_SAMPLES) {
            const float phase = goertzel_phase(&state->bin);
            if (state->has_phase) {
                const float expected =
                    MATH_TAU * state->nominal_hz * AUDIO_LOOPBACK_WINDOW_SAMPLES / AUDIO_OUT_SAMPLE_RATE_HZ;
                state->phase_error += (double)remainderf(phase - state->previous_phase - expected, MATH_TAU);
                state->advances++;
            }
            state->has_phase = true;
            state->previous_phase = phase;
            goertzel_reset(&state->bin);
        }
    }
}

float
audio_loopback_frequency_hz(const audio_loopback_frequency_t* state) {
    if (!state->advances) {
        return NAN;
    }
    return state->nominal_hz
           + (float)(state->phase_error * AUDIO_OUT_SAMPLE_RATE_HZ
                     / ((double)MATH_TAU * AUDIO_LOOPBACK_WINDOW_SAMPLES * (double)state->advances));
}

#ifdef ESP_PLATFORM
bool
audio_loopback_measure(uint16_t hz, uint16_t ms, float* db, float* measured_hz) {
    if (!db || !audio_out_args_valid(hz, ms, 0)) {
        return false;
    }
    int16_t in[AUDIO_CODEC_BLOCK_SAMPLES];
    goertzel_t detector;
    goertzel_init(&detector, (float)hz, (float)AUDIO_OUT_SAMPLE_RATE_HZ);
    audio_loopback_frequency_t frequency;
    audio_loopback_frequency_init(&frequency, hz);
    size_t remaining = audio_loopback_sample_budget(ms);
    size_t settle = audio_out_samples(AUDIO_LOOPBACK_SETTLE_MS);
    bool ok = true;
    while (remaining && ok) {
        const size_t n = remaining < AUDIO_CODEC_BLOCK_SAMPLES ? remaining : AUDIO_CODEC_BLOCK_SAMPLES;
        ok = audio_in_read(in, n);
        if (ok) {
            const size_t skipped = settle < n ? settle : n;
            settle -= skipped;
            goertzel_feed_s(&detector, in + skipped, n - skipped);
            audio_loopback_frequency_feed(&frequency, in + skipped, n - skipped);
            remaining -= n;
        }
    }
    *db = goertzel_level_db(&detector, INT16_MAX);
    if (measured_hz) {
        *measured_hz = audio_loopback_frequency_hz(&frequency);
    }
    return ok;
}
#endif
