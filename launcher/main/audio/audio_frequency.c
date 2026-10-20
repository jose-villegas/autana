#include "audio/audio_frequency.h"

#include <math.h>

#include "math/scalar/math_const.h"

void
audio_frequency_init(audio_frequency_t* state, uint16_t hz) {
    *state = (audio_frequency_t){.nominal_hz = (float)hz};
    goertzel_init(&state->bin, (float)hz, (float)AUDIO_OUT_SAMPLE_RATE_HZ);
}

void
audio_frequency_feed(audio_frequency_t* state, const int16_t* samples, size_t count) {
    while (count) {
        const size_t space = AUDIO_FREQUENCY_WINDOW_SAMPLES - state->bin.samples;
        const size_t n = count < space ? count : space;
        goertzel_feed_s(&state->bin, samples, n);
        samples += n;
        count -= n;
        if (state->bin.samples == AUDIO_FREQUENCY_WINDOW_SAMPLES) {
            const float phase = goertzel_phase(&state->bin);
            if (state->has_phase) {
                const float expected =
                    MATH_TAU * state->nominal_hz * AUDIO_FREQUENCY_WINDOW_SAMPLES / AUDIO_OUT_SAMPLE_RATE_HZ;
                const float error =
                    remainderf(phase - state->previous_phase - expected, MATH_TAU) - state->phase_error_correction;
                const float sum = state->phase_error + error;
                state->phase_error_correction = (sum - state->phase_error) - error;
                state->phase_error = sum;
                state->advances++;
            }
            state->has_phase = true;
            state->previous_phase = phase;
            goertzel_reset(&state->bin);
        }
    }
}

float
audio_frequency_hz(const audio_frequency_t* state) {
    if (!state->advances || state->nominal_hz < AUDIO_FREQUENCY_EDGE_HZ
        || state->nominal_hz > ((float)AUDIO_OUT_SAMPLE_RATE_HZ / 2) - AUDIO_FREQUENCY_EDGE_HZ) {
        return NAN;
    }
    return state->nominal_hz
           + (state->phase_error * AUDIO_OUT_SAMPLE_RATE_HZ
              / (MATH_TAU * AUDIO_FREQUENCY_WINDOW_SAMPLES * (float)state->advances));
}
