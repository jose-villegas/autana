/*
 * maths: int16 scalar operations for the math templates (math_template.h),
 * compact storage for a low-memory app's coordinates. Sums and products
 * SATURATE at the int16 range, so a vector never wraps; a dot product widens
 * to int64. There is no division, square root or angle: convert to a float
 * or fixed vector to rotate.
 */
#pragma once

#include <stdint.h>

static inline int16_t
maths_saturate(int32_t v) {
    if (v > INT16_MAX) {
        return INT16_MAX;
    }
    return v < INT16_MIN ? INT16_MIN : (int16_t)v;
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
