/*
 * ridge_motion - what the launcher's ridge does when nobody touches it. It
 * breathes: its shape eases toward a smoothed copy of itself and back to the
 * rigid original. A wave runs along it. And the wave has momentum: while the
 * device turns the line lags true level, the ridge is for that moment a
 * slope, and the wave is pushed down it and coasts on after.
 *
 * Pure: time and the slope are passed in, heights come out. Q4 heights, as
 * gfx_glow.h takes them; phases are trig.h's, 65536 to the turn.
 */
#pragma once

#include <stdint.h>

#include "util/trig.h"

/* One breath, and how far toward the smoothed shape it goes, out of 256. */
#define RIDGE_BREATH_MS         9000
#define RIDGE_BREATH_DEPTH      200

/* How far the smoothed shape looks to either side, in columns, and how many
 * times over: three box passes are close to a Gaussian. */
#define RIDGE_SMOOTH_RADIUS     20
#define RIDGE_SMOOTH_PASSES     3

/* The wave: height in Q4, length in columns, and how long one takes to pass
 * a point with no slope pushing it. */
#define RIDGE_WAVE_HEIGHT_Q4    40
#define RIDGE_WAVE_LENGTH       170
#define RIDGE_WAVE_PASSES_IN_MS 2600

/* Momentum is wave speed, in Q8 phase per millisecond. A slope of 1.0 (Q14)
 * adds RIDGE_PUSH of it per millisecond, and it loses 1/RIDGE_COAST_MS of
 * itself per millisecond, so it coasts for about that long. */
#define RIDGE_PUSH              96
#define RIDGE_COAST_MS          900
#define RIDGE_SPEED_MAX         (120 << 8)

typedef struct {
    uint32_t breath_ms;
    uint32_t wave_phase_q8; /* the wave's phase at column 0, Q8 */
    int32_t momentum_q8;
} ridge_motion_t;

/* `slope_q14` is how steeply the ridge runs downhill toward its last column,
 * -1.0 to 1.0: the sine of the angle between where the line is and level. */
static inline void
ridge_motion_advance(ridge_motion_t* motion, uint32_t dt_ms, int32_t slope_q14) {
    motion->breath_ms = (motion->breath_ms + dt_ms) % RIDGE_BREATH_MS;

    int64_t momentum = motion->momentum_q8;
    momentum += (int64_t)slope_q14 * RIDGE_PUSH * (int64_t)dt_ms / 16384;
    /* A share of a small momentum rounds to nothing, and the wave would run
     * a little fast for ever after one tilt: it always loses at least 1. */
    int64_t lost = momentum * (int64_t)dt_ms / RIDGE_COAST_MS;
    if (lost == 0 && momentum != 0 && dt_ms > 0) {
        lost = momentum > 0 ? 1 : -1;
    }
    momentum -= lost;
    momentum = momentum > RIDGE_SPEED_MAX ? RIDGE_SPEED_MAX : momentum;
    momentum = momentum < -RIDGE_SPEED_MAX ? -RIDGE_SPEED_MAX : momentum;
    motion->momentum_q8 = (int32_t)momentum;

    const int64_t own_speed_q8 = ((int64_t)65536 << 8) / RIDGE_WAVE_PASSES_IN_MS;
    motion->wave_phase_q8 += (uint32_t)((own_speed_q8 + momentum) * (int64_t)dt_ms);
}

/* How far toward the smoothed shape, 0 to RIDGE_BREATH_DEPTH: nothing at the
 * top of each breath, so the ridge is its rigid self once a breath. */
static inline int
ridge_motion_breath(const ridge_motion_t* motion) {
    const uint16_t phase = (uint16_t)((uint64_t)motion->breath_ms * 65536 / RIDGE_BREATH_MS);
    const int32_t out = 32767 - trig_cos(phase); /* 0 .. 65534 */
    return (int)((int64_t)out * RIDGE_BREATH_DEPTH / 65534);
}

static inline int
ridge_motion_wave(const ridge_motion_t* motion, int x) {
    const uint32_t along = (uint32_t)x * (65536u / RIDGE_WAVE_LENGTH);
    const uint16_t phase = (uint16_t)((motion->wave_phase_q8 >> 8) - along);
    return (int)(trig_sin(phase) * RIDGE_WAVE_HEIGHT_Q4 / 32767);
}

static inline int16_t
ridge_motion_height(const ridge_motion_t* motion, int rigid, int smooth, int x) {
    const int breathed = rigid + (smooth - rigid) * ridge_motion_breath(motion) / 256;
    return (int16_t)(breathed + ridge_motion_wave(motion, x));
}

/* `out` is `in` smoothed; `scratch` is `count` long too. The ends repeat
 * their last value, so a flat line stays exactly where it is. */
static inline void
ridge_motion_smooth(const int16_t* in, int16_t* out, int16_t* scratch, int count) {
    const int16_t* from = in;
    for (int pass = 0; pass < RIDGE_SMOOTH_PASSES; pass++) {
        int16_t* to = (pass % 2 == 0) ? out : scratch;
        for (int x = 0; x < count; x++) {
            int32_t sum = 0;
            for (int k = -RIDGE_SMOOTH_RADIUS; k <= RIDGE_SMOOTH_RADIUS; k++) {
                const int j = x + k < 0 ? 0 : (x + k >= count ? count - 1 : x + k);
                sum += from[j];
            }
            to[x] = (int16_t)(sum / (2 * RIDGE_SMOOTH_RADIUS + 1));
        }
        from = to;
    }
    if (from != out) {
        for (int x = 0; x < count; x++) {
            out[x] = from[x];
        }
    }
}
