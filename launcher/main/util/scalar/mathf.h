/*
 * mathf: single-precision float's scalar operations for the math templates
 * (math_template.h). The FPU has no double, so one stray promotion costs an
 * order of magnitude; the build refuses the promotion across the whole main
 * component (main/CMakeLists.txt). Angles are radians. Needs libm for sqrtf,
 * sinf, cosf and acosf. A `w` operation is the one a dot product sums with.
 */
#pragma once

#include <math.h>

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

typedef float mathf_wide_t;
typedef float mathf_divisor_t;

static inline mathf_wide_t
mathf_product_difference(float a, float b, float c, float d) {
    return (a * b) - (c * d);
}

static inline mathf_divisor_t
mathf_dot3_divisor(float a0, float b0, float a1, float b1, float a2, float b2) {
    return 1.0F / (((a0 * b0) + (a1 * b1)) + (a2 * b2));
}

static inline float
mathf_divide_wide(float numerator, mathf_divisor_t divisor) {
    return numerator * divisor;
}
