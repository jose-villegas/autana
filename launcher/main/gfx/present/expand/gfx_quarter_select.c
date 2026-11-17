#include "gfx/present/expand/gfx_quarter_select.h"

#include <stdbool.h>

#include "image/pixel/rgb565.h"
#include "simd/simd.h"

/* Pixel x's pick: mine where its bit in `keep` is set, else the quarter step. */
static uint16_t
quarter_or_kept(uint16_t mine, uint16_t other, const uint8_t* keep, int x) {
    const bool kept = (keep[x / GFX_QUARTER_LANES] >> (x % GFX_QUARTER_LANES)) & 1U;
    return kept ? mine : rgb565be_unspread(rgb565_spread_quarter(rgb565be_spread(mine), rgb565be_spread(other)));
}

void
gfx_quarter_select_scalar(const uint16_t* mine, const uint16_t* other, const uint8_t* keep, uint16_t* out, int steps) {
    for (int x = 0; x < steps * GFX_QUARTER_LANES; x++) {
        out[x] = quarter_or_kept(mine[x], other[x], keep, x);
    }
}

void
gfx_quarter_spread_scalar(const uint16_t* mine, const uint16_t* left, const uint16_t* right, const uint8_t* keeps,
                          uint16_t* out, int steps) {
    for (int x = 0; x < steps * GFX_QUARTER_LANES; x++) {
        out[2 * x] = quarter_or_kept(mine[x], left[x], keeps, x);
        out[(2 * x) + 1] = quarter_or_kept(mine[x], right[x], keeps + steps, x);
    }
}

#if !SIMD_PIE
void
gfx_quarter_select(const uint16_t* mine, const uint16_t* other, const uint8_t* keep, uint16_t* out, int steps) {
    gfx_quarter_select_scalar(mine, other, keep, out, steps);
}

void
gfx_quarter_spread(const uint16_t* mine, const uint16_t* left, const uint16_t* right, const uint8_t* keeps,
                   uint16_t* out, int steps) {
    gfx_quarter_spread_scalar(mine, left, right, keeps, out, steps);
}
#endif
