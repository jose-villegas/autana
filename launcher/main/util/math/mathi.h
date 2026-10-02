/*
 * mathi: plain int32 scalar operations for the math templates
 * (math_template.h), for grid, pixel and cell coordinates. Sums and products
 * wrap like any int32, so a caller keeps its coordinates small; a dot
 * product widens to int64. There is no division, square root or angle, hence
 * no rotation maths.
 */
#pragma once

#include <stdint.h>

static inline int32_t
mathi_zero(void) {
    return 0;
}

static inline int32_t
mathi_one(void) {
    return 1;
}

static inline int32_t
mathi_add(int32_t a, int32_t b) {
    return (int32_t)((uint32_t)a + (uint32_t)b);
}

static inline int32_t
mathi_sub(int32_t a, int32_t b) {
    return (int32_t)((uint32_t)a - (uint32_t)b);
}

static inline int32_t
mathi_neg(int32_t a) {
    return (int32_t)(0u - (uint32_t)a);
}

static inline int32_t
mathi_mul(int32_t a, int32_t b) {
    return (int32_t)((uint32_t)a * (uint32_t)b);
}

static inline int64_t
mathi_wadd(int64_t a, int64_t b) {
    return a + b;
}

static inline int64_t
mathi_wmul(int32_t a, int32_t b) {
    return (int64_t)a * (int64_t)b;
}
