/* bayer: ordered dithering at absolute pixel coordinates. */
#pragma once

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

/* The matrix's side, and its levels: one threshold per cell. */
#define BAYER4_SIDE   4
#define BAYER4_LEVELS (BAYER4_SIDE * BAYER4_SIDE)

/* Absolute pixel coordinates keep overlapping or adjacent shapes in phase;
 * local coordinates would restart the pattern and create seams. */
static const uint8_t bayer4x4[BAYER4_SIDE][BAYER4_SIDE] = {
    {0, 8, 2, 10},
    {12, 4, 14, 6},
    {3, 11, 1, 9},
    {15, 7, 13, 5},
};

/* Nearest coverage level for value out of whole, with exact half levels
 * rounded down. A whole value covers every matrix cell. */
static inline int
bayer_level(uint32_t value, uint32_t whole) {
    assert(whole > 0);
    if (value >= whole) {
        return BAYER4_LEVELS;
    }
    return (int)(((uint64_t)value * BAYER4_LEVELS + (whole - 1U) / 2U) / whole);
}

/* Share absolute pixel coordinates and rounding to keep dithered shapes in phase. */
static inline bool
bayer_covers(int x, int y, uint8_t alpha) {
    return bayer_level(alpha, UINT8_MAX) > bayer4x4[y & (BAYER4_SIDE - 1)][x & (BAYER4_SIDE - 1)];
}
