/* smooth: exact exponential approach toward a target, independent of frame step. */
#pragma once

#include <math.h>

/* Share of the gap closed: dt >= 0, tau > 0, same time unit.
 * expm1f preserves precision for small steps. */
static inline float
smooth_alpha(float dt, float tau) {
    return -expm1f(-dt / tau);
}

/* Share left: dt >= 0, half_life > 0, same time unit. */
static inline float
smooth_decay(float dt, float half_life) {
    return exp2f(-dt / half_life);
}
