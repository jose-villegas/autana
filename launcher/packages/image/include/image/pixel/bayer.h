/* bayer: ordered dithering at absolute pixel coordinates. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Absolute pixel coordinates keep overlapping or adjacent shapes in phase;
 * local coordinates would restart the pattern and create seams. */
static const uint8_t bayer4x4[4][4] = {
    {0, 8, 2, 10},
    {12, 4, 14, 6},
    {3, 11, 1, 9},
    {15, 7, 13, 5},
};

/* A caller with a whole row at one alpha computes the level once.
 * +8 rounds to the nearest level; truncation would cover nothing for alpha 1..15.
 * Alpha 255 must exceed every matrix cell to keep full coverage. */
static inline int
bayer_level(uint8_t alpha) {
    if (alpha == 0) {
        return 0;
    }
    if (alpha >= 255) {
        return 16;
    }
    return (alpha + 8) >> 4;
}

/* Share absolute pixel coordinates and rounding to keep dithered shapes in phase. */
static inline bool
bayer_covers(int x, int y, uint8_t alpha) {
    return bayer_level(alpha) > bayer4x4[y & 3][x & 3];
}
