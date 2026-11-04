/* smooth: exact exponential approach toward a target, independent of frame step. */
#pragma once

#include <math.h>

/* Share of the gap closed after dt, with time constant tau.
 * expm1f preserves precision for small steps. */
static inline float
smooth_alpha(float dt, float tau) {
    return -expm1f(-dt / tau);
}

/* Share left after dt of anything halving every half_life. */
static inline float
smooth_decay(float dt, float half_life) {
    return exp2f(-dt / half_life);
}
