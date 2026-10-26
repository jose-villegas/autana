/* bayer: ordered dithering at absolute pixel coordinates. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* The standard order-4 Bayer matrix, values 0..15 rather than pre-scaled
 * to 0..255; bayer_covers() scales the ONE side that needs to be a
 * byte (alpha), not the table it is compared against. Indexed by each
 * pixel's own ABSOLUTE panel row/col (y & 3, x & 3), not a position
 * local to whatever shape is dithered; that keeps two dithered shapes
 * that overlap or sit edge to edge in phase: a local index would have
 * every shape restart the pattern at its own corner, reading as a seam
 * where two meet. */
static const uint8_t bayer4x4[4][4] = {
    {0, 8, 2, 10},
    {12, 4, 14, 6},
    {3, 11, 1, 9},
    {15, 7, 13, 5},
};

/* The alpha -> Bayer-level scaling bayer_covers() compares against
 * the table, exposed on its own so a caller with a whole ROW of pixels
 * at one alpha can compute the level once instead of per pixel; see
 * boot_anim.c's draw_image(). Rounded, not truncated: a plain
 * `alpha >> 4` maps every alpha in 1..15 to level 0, covering nothing;
 * +8 rounds to the nearest of 16 levels instead of flooring. 17, not 16:
 * alpha 255 maps above every table cell (max 15), keeping "fully solid"
 * exactly solid. */
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

/* Whether an ordered (Bayer) dither at `alpha` (0 nothing, 255 everything,
 * 16 graduated levels between; see draw_rect_dither()'s own comment
 * in draw.c) covers absolute panel pixel (x, y). Callers with their own pixel
 * loops share this table and rounding rule rather than drifting out of phase. */
static inline bool
bayer_covers(int x, int y, uint8_t alpha) {
    return bayer_level(alpha) > bayer4x4[y & 3][x & 3];
}
