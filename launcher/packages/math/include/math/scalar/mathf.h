/*
 * mathf: single-precision float's scalar operations for the math templates
 * (docs/math/README.md). The FPU has no double, so one stray promotion costs an
 * order of magnitude; main and every package refuse the promotion in their
 * CMakeLists.txt. Angles are radians. Needs libm for sqrtf,
 * sinf and cosf. A `w` operation is the one a dot product sums with.
 */
#pragma once

#include <assert.h>
#include <math.h>

#ifdef __XTENSA__
#include "sdkconfig.h"
#endif

static inline float
mathf_zero(void) {
    return 0.0F;
}

static inline float
mathf_one(void) {
    return 1.0F;
}

static inline float
mathf_two(void) {
    return 2.0F;
}

static inline float
mathf_four(void) {
    return 4.0F;
}

static inline float
mathf_clamp(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline float
mathf_add(float a, float b) {
    return a + b;
}

static inline float
mathf_sub(float a, float b) {
    return a - b;
}

static inline float
mathf_neg(float a) {
    return -a;
}

static inline float
mathf_mul(float a, float b) {
    return a * b;
}

static inline float
mathf_div(float a, float b) {
    return a / b;
}

/* 1 / a for a nonzero a away from float's range limits. The S3's `/` is a
 * libgcc routine of about 56 cycles; its FPU's reciprocal seed and two Newton
 * steps take about 18 and land within one ulp of the quotient, on it 99.9% of
 * the time. CONFIG_MATH_FPU_RECIP enables the seed; without it, division
 * is exact, as on other targets. */
static inline float
mathf_recip(float a) {
#if defined(__XTENSA__) && CONFIG_MATH_FPU_RECIP
    float y;
    __asm__("recip0.s %0, %1" : "=f"(y) : "f"(a));
    y += y * (1.0F - (a * y));
    return y + (y * (1.0F - (a * y)));
#else
    return 1.0F / a;
#endif
}

static inline float
mathf_sqrt(float a) {
    return sqrtf(a);
}

static inline float
mathf_half_sin(float angle) {
    return sinf(angle * 0.5F);
}

static inline float
mathf_half_cos(float angle) {
    return cosf(angle * 0.5F);
}

/* a0 * b0 + a1 * b1 + c, summed left to right. */
static inline float
mathf_dot2c(float a0, float b0, float a1, float b1, float c) {
    return ((a0 * b0) + (a1 * b1)) + c;
}

/* a0 * b0 + a1 * b1 + a2 * b2 + c, summed left to right. */
static inline float
mathf_dot3c(float a0, float b0, float a1, float b1, float a2, float b2, float c) {
    return (((a0 * b0) + (a1 * b1)) + (a2 * b2)) + c;
}

static inline float
mathf_wadd(float a, float b) {
    return a + b;
}

static inline float
mathf_wmul(float a, float b) {
    return a * b;
}

static inline float
mathf_lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

static inline float
mathf_min(float a, float b) {
    return a < b ? a : b;
}

static inline float
mathf_max(float a, float b) {
    return a < b ? b : a;
}

static inline float
mathf_repeat(float v, float len) {
    assert(len > 0.0F);
    float r = fmodf(v, len);
    if (r < 0.0F) {
        r += len;
    }
    /* A tiny negative input can round the remainder up to the excluded endpoint. */
    return r < len ? r : 0.0F;
}

static inline float
mathf_wrap(float v, float lo, float hi) {
    assert(lo < hi);
    const float wrapped = lo + mathf_repeat(v - lo, hi - lo);
    return wrapped < hi ? wrapped : lo;
}
