/* Float recurrence avoids fixed-point overflow on long streaming captures
 * and uses the S3's single-precision FPU. */
#include "audio/audio_goertzel.h"

#include <math.h>

#include "math/scalar/trig.h"

#define GOERTZEL_TAU             6.2831853071795864769F
#define GOERTZEL_POWER_DB_SCALE  10.0F
#define GOERTZEL_SINE_COMPONENTS 2.0F

void
audio_goertzel_init(audio_goertzel_t* state, uint16_t hz, uint32_t sample_rate) {
    /* Q15 table coefficients detune long records; cosf preserves the requested bin. */
    *state = (audio_goertzel_t){.coefficient =
                                    GOERTZEL_SINE_COMPONENTS * cosf(GOERTZEL_TAU * (float)hz / (float)sample_rate)};
}

void
audio_goertzel_feed(audio_goertzel_t* state, const int16_t* samples, size_t n) {
    for (size_t i = 0; i < n; i++) {
        const float next = (float)samples[i] + (state->coefficient * state->previous) - state->previous2;
        state->previous2 = state->previous;
        state->previous = next;
    }
    state->samples += n;
}

float
audio_goertzel_power(const audio_goertzel_t* state) {
    return fmaxf(0.0F, (state->previous * state->previous) + (state->previous2 * state->previous2)
                           - (state->coefficient * state->previous * state->previous2));
}

float
audio_goertzel_dbfs(const audio_goertzel_t* state) {
    const float power = audio_goertzel_power(state);
    if (state->samples == 0 || power <= 0.0F) {
        return AUDIO_GOERTZEL_FLOOR_DB;
    }
    const float scale = (float)state->samples * TRIG_SIN_MAX / GOERTZEL_SINE_COMPONENTS;
    return fmaxf(AUDIO_GOERTZEL_FLOOR_DB, GOERTZEL_POWER_DB_SCALE * log10f(power / (scale * scale)));
}
