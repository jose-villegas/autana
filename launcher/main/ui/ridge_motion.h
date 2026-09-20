/*
 * ridge_motion - what the launcher's ridge does when nobody touches it. It
 * breathes: its shape eases toward a smoothed copy of itself and back to the
 * rigid original. A wave runs along it. And the wave has momentum: while the
 * device turns the line lags true level, the ridge is for that moment a
 * slope, and the wave is pushed down it and coasts on after.
 *
 * Pure: time, the slope and how much of each are passed in, heights come
 * out. Q4 heights, as gfx_glow.h takes them; phases are trig.h's, 65536 to
 * the turn.
 */
#pragma once

#include <stdint.h>

#include "util/trig.h"

/* How much of each. Passed in rather than compiled in because these are
 * judged by eye, on the device, and a caller may be changing them live. */
typedef struct {
    int breath_ms;    /* one breath */
    int breath_depth; /* how far toward the smoothed shape, out of 256 */
    int wave_height_q4;
    int wave_length;       /* columns */
    int wave_passes_in_ms; /* how long one wave takes to pass a point, unpushed */
    int push;              /* Q8 phase per ms of momentum a slope of 1.0 adds per ms */
    int coast_ms;          /* momentum loses 1/coast_ms of itself per ms */
} ridge_motion_params_t;

/* How far the smoothed shape looks to either side, in columns, by default,
 * and how many times over: three box passes are close to a Gaussian. */
#define RIDGE_SMOOTH_RADIUS 20
#define RIDGE_SMOOTH_PASSES 3

/* Momentum is wave speed, in Q8 phase per millisecond. */
#define RIDGE_SPEED_MAX     (120 << 8)

typedef struct {
    uint32_t breath_ms;
    uint32_t wave_phase_q8; /* the wave's phase at column 0, Q8 */
    int32_t momentum_q8;
} ridge_motion_t;

/* `slope_q14` is how steeply the ridge runs downhill toward its last column,
 * -1.0 to 1.0: the sine of the angle between where the line is and level. */
static inline void
ridge_motion_advance(ridge_motion_t* motion, const ridge_motion_params_t* params, uint32_t dt_ms, int32_t slope_q14) {
    const uint32_t breath_ms = params->breath_ms > 0 ? (uint32_t)params->breath_ms : 1;
    motion->breath_ms = (motion->breath_ms + dt_ms) % breath_ms;

    int64_t momentum = motion->momentum_q8;
    momentum += (int64_t)slope_q14 * params->push * (int64_t)dt_ms / 16384;
    /* A share of a small momentum rounds to nothing, and the wave would run
     * a little fast for ever after one tilt: it always loses at least 1. */
    int64_t lost = momentum * (int64_t)dt_ms / (params->coast_ms > 0 ? params->coast_ms : 1);
    if (lost == 0 && momentum != 0 && dt_ms > 0) {
        lost = momentum > 0 ? 1 : -1;
    }
    momentum -= lost;
    momentum = momentum > RIDGE_SPEED_MAX ? RIDGE_SPEED_MAX : momentum;
    momentum = momentum < -RIDGE_SPEED_MAX ? -RIDGE_SPEED_MAX : momentum;
    motion->momentum_q8 = (int32_t)momentum;

    const int64_t passes_in_ms = params->wave_passes_in_ms > 0 ? params->wave_passes_in_ms : 1;
    const int64_t own_speed_q8 = ((int64_t)65536 << 8) / passes_in_ms;
    motion->wave_phase_q8 += (uint32_t)((own_speed_q8 + momentum) * (int64_t)dt_ms);
}

/* How much of its motion the line has, out of 256, `elapsed_ms` into taking
 * `over_ms` to come by all of it: slow away from stiff and slow into full, so
 * neither end of the hand-over shows as a start or a stop. */
static inline int
ridge_motion_ease_in(uint32_t elapsed_ms, uint32_t over_ms) {
    if (over_ms == 0 || elapsed_ms >= over_ms) {
        return 256;
    }
    const int64_t t = (int64_t)elapsed_ms * 256 / over_ms;
    return (int)((t * t * (3 * 256 - 2 * t)) >> 16);
}

/* How far toward the smoothed shape, 0 to the depth: nothing at the top of
 * each breath, so the ridge is its rigid self once a breath. */
static inline int
ridge_motion_breath(const ridge_motion_t* motion, const ridge_motion_params_t* params) {
    const uint64_t breath_ms = params->breath_ms > 0 ? (uint64_t)params->breath_ms : 1;
    const uint16_t phase = (uint16_t)((uint64_t)motion->breath_ms * 65536 / breath_ms);
    const int32_t out = 32767 - trig_cos(phase); /* 0 .. 65534 */
    return (int)((int64_t)out * params->breath_depth / 65534);
}

static inline int
ridge_motion_wave(const ridge_motion_t* motion, const ridge_motion_params_t* params, int x) {
    const uint32_t length = params->wave_length > 0 ? (uint32_t)params->wave_length : 1;
    const uint32_t along = (uint32_t)x * (65536u / length);
    const uint16_t phase = (uint16_t)((motion->wave_phase_q8 >> 8) - along);
    return (int)(trig_sin(phase) * params->wave_height_q4 / 32767);
}

static inline int16_t
ridge_motion_height(const ridge_motion_t* motion, const ridge_motion_params_t* params, int rigid, int smooth, int x) {
    const int breathed = rigid + (smooth - rigid) * ridge_motion_breath(motion, params) / 256;
    return (int16_t)(breathed + ridge_motion_wave(motion, params, x));
}

/* How many columns the slope at an end is read over. */
#define RIDGE_END_SLOPE_SPAN 12

/* `out`, `count + 2 * extra` long, is `in` with `extra` more columns at each
 * end: each end carries on at the slope it had and eases level, the way a
 * slope runs out into a plain. Turned to a diagonal the line has to span the
 * panel's diagonal, which is longer than the frame it was drawn in. The
 * middle is `in` exactly, and stays centred. */
static inline void
ridge_motion_extend(const int16_t* in, int count, int16_t* out, int extra) {
    for (int x = 0; x < count; x++) {
        out[extra + x] = in[x];
    }
    const int span = count > RIDGE_END_SLOPE_SPAN ? RIDGE_END_SLOPE_SPAN : count - 1;
    if (extra <= 0 || span <= 0) {
        return;
    }
    const int left_per_span = in[0] - in[span];
    const int right_per_span = in[count - 1] - in[count - 1 - span];
    for (int k = 1; k <= extra; k++) {
        /* k columns out at a slope falling linearly to nothing at `extra`. */
        const int64_t run = (int64_t)k * (2 * extra - k);
        out[extra - k] = (int16_t)(in[0] + left_per_span * run / ((int64_t)2 * extra * span));
        out[extra + count - 1 + k] = (int16_t)(in[count - 1] + right_per_span * run / ((int64_t)2 * extra * span));
    }
}

/* `out` is `in` smoothed over `radius` columns to either side; `scratch` is
 * `count` long too. The ends repeat their last value, so a flat line stays
 * exactly where it is. */
static inline void
ridge_motion_smooth(const int16_t* in, int16_t* out, int16_t* scratch, int count, int radius) {
    const int16_t* from = in;
    if (radius < 0) {
        radius = 0;
    }
    for (int pass = 0; pass < RIDGE_SMOOTH_PASSES; pass++) {
        int16_t* to = (pass % 2 == 0) ? out : scratch;
        for (int x = 0; x < count; x++) {
            int32_t sum = 0;
            for (int k = -radius; k <= radius; k++) {
                const int j = x + k < 0 ? 0 : (x + k >= count ? count - 1 : x + k);
                sum += from[j];
            }
            to[x] = (int16_t)(sum / (2 * radius + 1));
        }
        from = to;
    }
    if (from != out) {
        for (int x = 0; x < count; x++) {
            out[x] = from[x];
        }
    }
}
