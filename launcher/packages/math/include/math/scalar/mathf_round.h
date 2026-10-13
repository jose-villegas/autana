/* mathf_round: scalar rounding to nearest, ties away from zero; finite inputs saturate to int32. */
#pragma once

#include <stdint.h>

/* The largest float below 2^31, so a clamped value still converts. */
#define MATH_FLOAT_INT_LIMIT 2147483520.0F

/* Finite ordered bounds; NaN maps to lo and infinities to the corresponding bound. */
static inline float
mathf_clamp_finite(float v, float lo, float hi) {
    return v > lo ? (v < hi ? v : hi) : lo;
}

static inline int32_t
mathf_round_i32(float v) {
    if (v >= MATH_FLOAT_INT_LIMIT) {
        return INT32_MAX;
    }
    if (v <= -MATH_FLOAT_INT_LIMIT) {
        return INT32_MIN;
    }
    const int32_t whole = (int32_t)v;
    const float fraction = v - (float)whole;
    return whole + (fraction >= 0.5F ? 1 : fraction <= -0.5F ? -1 : 0);
}
