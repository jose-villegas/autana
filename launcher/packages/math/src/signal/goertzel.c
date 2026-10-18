/* Float recurrence avoids fixed-point overflow on long streams. */
#include "math/signal/goertzel.h"

#include <math.h>

#include "math/linear/math_const.h"

#define GOERTZEL_POWER_DB_SCALE  10.0F
#define GOERTZEL_SINE_COMPONENTS 2.0F

void
goertzel_init(goertzel_t* state, float hz, float sample_rate_hz) {
    /* Q15 table coefficients detune long records; cosf preserves the requested bin. */
    *state = (goertzel_t){.coefficient = GOERTZEL_SINE_COMPONENTS * cosf(MATH_TAU * hz / sample_rate_hz)};
}

void
goertzel_reset(goertzel_t* state) {
    *state = (goertzel_t){.coefficient = state->coefficient};
}

static void
goertzel_step(goertzel_t* state, float sample) {
    const float next = sample + (state->coefficient * state->previous) - state->previous2;
    state->previous2 = state->previous;
    state->previous = next;
}

void
goertzel_feed_s(goertzel_t* state, const int16_t* samples, size_t n) {
    for (size_t i = 0; i < n; i++) {
        goertzel_step(state, (float)samples[i]);
    }
    state->samples += n;
}

void
goertzel_feed_f(goertzel_t* state, const float* samples, size_t n) {
    for (size_t i = 0; i < n; i++) {
        goertzel_step(state, samples[i]);
    }
    state->samples += n;
}

float
goertzel_power(const goertzel_t* state) {
    return fmaxf(0.0F, (state->previous * state->previous) + (state->previous2 * state->previous2)
                           - (state->coefficient * state->previous * state->previous2));
}

float
goertzel_amplitude(const goertzel_t* state) {
    return state->samples == 0 ? 0.0F : GOERTZEL_SINE_COMPONENTS * sqrtf(goertzel_power(state)) / (float)state->samples;
}

float
goertzel_level_db(const goertzel_t* state, float full_scale) {
    const float power = goertzel_power(state);
    if (state->samples == 0 || power <= 0.0F) {
        return GOERTZEL_LEVEL_FLOOR_DB;
    }
    const float scale = (float)state->samples * full_scale / GOERTZEL_SINE_COMPONENTS;
    return fmaxf(GOERTZEL_LEVEL_FLOOR_DB, GOERTZEL_POWER_DB_SCALE * log10f(power / (scale * scale)));
}
