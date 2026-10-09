/*
 * mathx: Q16.16 fixed-point scalar operations for the math templates
 * (math_template.h).
 *
 * Operations saturate, except the mathx_dot2c(), mathx_dot3c() and
 * mathx_dot3_narrow() fast paths, which wrap and floor; saturation turns
 * overflow into a bounded coordinate. A divide by zero saturates by the
 * numerator's sign (0 / 0 is 0). Other products round to nearest, ties away
 * from zero. Angles are Q16.16 turns whose low 16 bits are util/scalar/trig.h phases;
 * a sine or cosine of +-1.0 comes back as +-65534. Header-only, static
 * inline and ESP-IDF-free.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "util/scalar/fixed.h"
#include "util/scalar/mathi.h"
#include "util/scalar/trig.h"

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
    return mathx_saturate(fx_div_round_wide(num, den, MATHX_SHIFT));
}

/* The square root of a non-negative value; a negative one gives 0. */
static inline int32_t
mathx_sqrt(int32_t v) {
    if (v <= 0) {
        return 0;
    }
    return (int32_t)mathi_isqrt64((uint64_t)v << MATHX_SHIFT);
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

/* a0 * b0 + a1 * b1 + a2 * b2 + c, then an arithmetic shift right by `shift`:
 * plain 32-bit products and sum, a multiply and an add each, for operands
 * whose whole sum fits in an int32. The caller proves that range; nothing
 * here saturates. */
static inline int32_t
mathx_dot3_narrow(int32_t a0, int32_t b0, int32_t a1, int32_t b1, int32_t a2, int32_t b2, int32_t c, int shift) {
    const uint32_t sum =
        ((uint32_t)a0 * (uint32_t)b0) + ((uint32_t)a1 * (uint32_t)b1) + ((uint32_t)a2 * (uint32_t)b2) + (uint32_t)c;
    return (int32_t)sum >> shift;
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
