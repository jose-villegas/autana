/*
 * trig, sine and cosine of a 16-bit phase, in integers: TRIG_TURN is one turn
 * and the result has TRIG_SIN_SHIFT fractional bits, at most TRIG_SIN_MAX. A
 * quarter-wave table of 65 entries, interpolated, which is 256 steps a turn
 * before interpolation and exact at the quarter points.
 *
 * `static inline`, like fixed.h beside it: some callers want one per point
 * drawn.
 */
#pragma once

#include <stdint.h>

#define TRIG_TURN_SHIFT   16
#define TRIG_TURN         (1u << TRIG_TURN_SHIFT)
#define TRIG_QUARTER_TURN (TRIG_TURN / 4)
#define TRIG_SIN_SHIFT    15
#define TRIG_SIN_MAX      ((1 << TRIG_SIN_SHIFT) - 1)

static const int16_t trig_sin_quarter[65] = {
    0,     804,   1608,  2410,  3212,  4011,  4808,  5602,  6393,  7179,  7962,  8739,  9512,
    10278, 11039, 11793, 12539, 13279, 14010, 14732, 15446, 16151, 16846, 17530, 18204, 18868,
    19519, 20159, 20787, 21403, 22005, 22594, 23170, 23731, 24279, 24811, 25329, 25832, 26319,
    26790, 27245, 27683, 28105, 28510, 28898, 29268, 29621, 29956, 30273, 30571, 30852, 31113,
    31356, 31580, 31785, 31971, 32137, 32285, 32412, 32521, 32609, 32678, 32728, 32757, 32767,
};

/* Interpolated, not snapped. Table: 256 steps, 1.4 degrees. Snapping errors
 * compound. */
static inline int32_t
trig_sin_quadrant(uint32_t r) {
    const uint32_t i = r >> 8;
    if (i >= 64) {
        return trig_sin_quarter[64];
    }
    const int32_t a = trig_sin_quarter[i];
    const int32_t b = trig_sin_quarter[i + 1];
    return a + (((b - a) * (int32_t)(r & 0xFF)) >> 8);
}

/* sin of a TRIG_TURN phase, scaled by TRIG_SIN_SHIFT. */
static inline int32_t
trig_sin(uint16_t phase) {
    const uint32_t quadrant = (uint32_t)phase >> (TRIG_TURN_SHIFT - 2);
    const uint32_t rest = (uint32_t)phase & (TRIG_QUARTER_TURN - 1);

    switch (quadrant) {
        case 0: return trig_sin_quadrant(rest);
        case 1: return trig_sin_quadrant(TRIG_QUARTER_TURN - rest);
        case 2: return -trig_sin_quadrant(rest);
        default: return -trig_sin_quadrant(TRIG_QUARTER_TURN - rest);
    }
}

static inline int32_t
trig_cos(uint16_t phase) {
    return trig_sin((uint16_t)(phase + TRIG_QUARTER_TURN));
}
