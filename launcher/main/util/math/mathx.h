/*
 * mathx: Q16.16 fixed-point scalar operations for the math templates
 * (math_template.h).
 *
 * Every operation SATURATES, except the dot2c and dot3c fast paths below: a result outside the type's range comes back as
 * its largest or smallest value instead of wrapping, so an overflow in a
 * transform shows as a clamped coordinate, never a wild one. A divide by zero
 * saturates by the numerator's sign (0 / 0 is 0). Products round to nearest,
 * ties away from zero, through util/fixed.h. Angles are in TURNS, Q16.16: one
 * turn is 65536, so the low 16 bits are util/trig.h's phase directly; sin and
 * cos are trig.h's 15-bit table scaled to Q16, so +-1.0 comes back as
 * +-65534. Header-only, static inline and ESP-IDF-free.
 */
#pragma once

#include <stdint.h>

#include "util/fixed.h"
#include "util/trig.h"

#define MATHX_SHIFT 16
#define MATHX_ONE   (1 << MATHX_SHIFT)

static inline int32_t
mathx_saturate(int64_t v) {
    if (v > INT32_MAX) {
        return INT32_MAX;
    }
    return v < INT32_MIN ? INT32_MIN : (int32_t)v;
}

static inline int32_t
mathx_add(int32_t a, int32_t b) {
    return mathx_saturate((int64_t)a + b);
}

static inline int32_t
mathx_sub(int32_t a, int32_t b) {
    return mathx_saturate((int64_t)a - b);
}

static inline int32_t
mathx_neg(int32_t a) {
    return mathx_saturate(-(int64_t)a);
}

static inline int32_t
mathx_mul(int32_t a, int32_t b) {
    return mathx_saturate(fx_round_shift((int64_t)a * b, MATHX_SHIFT));
}

static inline int32_t
mathx_div(int32_t num, int32_t den) {
    if (den == 0) {
        return num > 0 ? INT32_MAX : (num < 0 ? INT32_MIN : 0);
    }
    const int64_t n = (int64_t)num * MATHX_ONE;
    const int64_t d = den < 0 ? -(int64_t)den : (int64_t)den;
    const int64_t q = ((n < 0 ? -n : n) + (d / 2)) / d;
    return mathx_saturate(((n < 0) != (den < 0)) ? -q : q);
}

/* The square root of a non-negative value; a negative one gives 0. */
static inline int32_t
mathx_sqrt(int32_t v) {
    if (v <= 0) {
        return 0;
    }
    const uint64_t n = (uint64_t)v << MATHX_SHIFT;
    uint64_t root = 0;
    for (uint64_t bit = (uint64_t)1 << 24; bit != 0; bit >>= 1) {
        if ((root + bit) * (root + bit) <= n) {
            root += bit;
        }
    }
    return (int32_t)root;
}

static inline int32_t
mathx_sin_turns(int32_t turns) {
    return trig_sin((uint16_t)turns) * 2;
}

static inline int32_t
mathx_cos_turns(int32_t turns) {
    return trig_cos((uint16_t)turns) * 2;
}

/* The dot products a matrix transform sums, as a fast path: each product is
 * floored to Q16.16 (a multiply and a funnel shift, no 64-bit sum, no sign
 * branch) and the 32-bit sum WRAPS, unlike mathx_add. Each term is off by
 * under one unit, so a sum by under three. For coordinates the caller keeps
 * inside +-32768, which a camera and a floor are. */
static inline int32_t
mathx_dot2c(int32_t a0, int32_t b0, int32_t a1, int32_t b1, int32_t c) {
    return (int32_t)((uint32_t)fx_mul_floor(a0, b0, MATHX_SHIFT) + (uint32_t)fx_mul_floor(a1, b1, MATHX_SHIFT)
                     + (uint32_t)c);
}

static inline int32_t
mathx_dot3c(int32_t a0, int32_t b0, int32_t a1, int32_t b1, int32_t a2, int32_t b2, int32_t c) {
    return (int32_t)((uint32_t)fx_mul_floor(a0, b0, MATHX_SHIFT) + (uint32_t)fx_mul_floor(a1, b1, MATHX_SHIFT)
                     + (uint32_t)fx_mul_floor(a2, b2, MATHX_SHIFT) + (uint32_t)c);
}

static inline int32_t
mathx_zero(void) {
    return 0;
}

static inline int32_t
mathx_one(void) {
    return MATHX_ONE;
}

static inline int32_t
mathx_two(void) {
    return 2 * MATHX_ONE;
}

static inline int32_t
mathx_four(void) {
    return 4 * MATHX_ONE;
}

static inline int32_t
mathx_half_sin(int32_t turns) {
    return mathx_sin_turns(turns / 2);
}

static inline int32_t
mathx_half_cos(int32_t turns) {
    return mathx_cos_turns(turns / 2);
}

static inline int32_t
mathx_wadd(int32_t a, int32_t b) {
    return mathx_add(a, b);
}

static inline int32_t
mathx_wmul(int32_t a, int32_t b) {
    return mathx_mul(a, b);
}
