/*
 * maths: int16 scalar operations for the math templates (docs/math/Scalar.md),
 * coordinates kept compact where memory is short. Sums and products
 * SATURATE at the int16 range, so a vector never wraps; a dot product widens
 * to int64. There is no division, square root or angle: convert to a float
 * or fixed vector to rotate.
 */
#pragma once

#include <stdint.h>

#include "math/scalar/mathi.h"

static inline int16_t
maths_saturate(int32_t v) {
    return (int16_t)mathi_min(mathi_max(v, INT16_MIN), INT16_MAX);
}

static inline int16_t
maths_zero(void) {
    return 0;
}

static inline int16_t
maths_one(void) {
    return 1;
}

static inline int16_t
maths_add(int16_t a, int16_t b) {
    return maths_saturate((int32_t)a + (int32_t)b);
}

static inline int16_t
maths_sub(int16_t a, int16_t b) {
    return maths_saturate((int32_t)a - (int32_t)b);
}

static inline int16_t
maths_neg(int16_t a) {
    return maths_saturate(-(int32_t)a);
}

static inline int16_t
maths_mul(int16_t a, int16_t b) {
    return maths_saturate((int32_t)a * (int32_t)b);
}

static inline int64_t
maths_wadd(int64_t a, int64_t b) {
    return a + b;
}

static inline int64_t
maths_wmul(int16_t a, int16_t b) {
    return (int64_t)a * (int64_t)b;
}

static inline int16_t
maths_lerp(int16_t a, int16_t b, int32_t t, int32_t n) {
    return (int16_t)mathi_lerp(a, b, t, n);
}

static inline int16_t
maths_min(int16_t a, int16_t b) {
    return (int16_t)mathi_min(a, b);
}

static inline int16_t
maths_max(int16_t a, int16_t b) {
    return (int16_t)mathi_max(a, b);
}

static inline int16_t
maths_clamp(int16_t v, int16_t lo, int16_t hi) {
    return (int16_t)mathi_clamp(v, lo, hi);
}

static inline int16_t
maths_repeat(int16_t v, int16_t len) {
    return (int16_t)mathi_repeat(v, len);
}

static inline int16_t
maths_wrap(int16_t v, int16_t lo, int16_t hi) {
    return (int16_t)mathi_wrap(v, lo, hi);
}
