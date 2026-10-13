/* tween: timeline ramps and easing. Time is plain milliseconds, since nothing
 * here needs finer; the fraction is a byte (0..255), since it only ever scales
 * a value and is divided by 255 again. Inline: some run once per drawn point. */
#pragma once

#include <stdint.h>

/* 0 before `start_ms`, 255 from `start_ms + dur_ms` on, linear between.
 * `dur_ms` of 0 jumps straight to 255 the instant `now_ms` passes
 * `start_ms`, rather than dividing by zero. */
static inline uint8_t
tween_ramp(uint32_t now_ms, uint32_t start_ms, uint32_t dur_ms) {
    if (now_ms <= start_ms) {
        return 0;
    }
    const uint32_t elapsed = now_ms - start_ms;
    if (dur_ms == 0 || elapsed >= dur_ms) {
        return 255;
    }
    return (uint8_t)((elapsed * 255u) / dur_ms);
}

/* Fast off the mark, settling as it arrives. Motion that starts and
 * stops at the same speed reads as mechanical; one squared term is
 * enough to stop it looking like a progress bar. Endpoints are exact
 * (0 stays 0, 255 stays 255), so composing this with tween_ramp() never
 * drifts a settled value off its target. */
static inline uint8_t
tween_ease_out(uint8_t linear) {
    const uint32_t left = 255u - linear;
    return (uint8_t)(255u - (left * left) / 255u);
}

/* Slow off the mark, fast by the end, mirror of tween_ease_out() above.
 * That belongs on motion arriving somewhere (slowing as it gets there);
 * this belongs on motion LEAVING somewhere it sat still, a swell
 * easing down off its own peak. Composed back to back (ease_out then
 * ease_in) the curves meet with a matching, near-zero rate of change,
 * reading as one smooth apex rather than a flat hold with a corner at
 * each end, the whole reason this exists over reusing tween_ease_out()
 * for both halves. */
static inline uint8_t
tween_ease_in(uint8_t linear) {
    return (uint8_t)(((uint32_t)linear * linear) / 255u);
}
