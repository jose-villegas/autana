/* smooth: exact exponential approach toward a target, independent of frame step. */
#pragma once

#include <math.h>
#include <stdint.h>

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

/* The weight of TCP's smoothed round-trip time (Paxson et al. 2011, RFC 6298
 * section 2.3): each sample moves the average by 1/8 of its distance. */
#define SMOOTH_RTT_DIVISOR 8

/* One step of an integer exponential average toward sample, weight
 * 1/divisor (divisor > 0), truncated toward zero. The caller seeds the
 * average with its first sample. */
static inline int64_t
smooth_step_i64(int64_t average, int64_t sample, int divisor) {
    return average + ((sample - average) / divisor);
}
