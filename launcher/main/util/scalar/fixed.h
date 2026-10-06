/*
 * fixed: shift-based fixed-point arithmetic at caller-selected scales.
 *
 * Products widen before shifting to avoid 32-bit overflow. Floor and round
 * are distinct: rounding a repeatedly negative accumulator changes its
 * trajectory. Geometry rounds to nearest with ties away from zero.
 * Header-only helpers keep innermost loops free of cross-file calls.
 */
#pragma once

#include <stdint.h>

/* Nearest integer, ties away from zero. An accumulator can contain several
 * products, so rounding is separate from multiplication. */
static inline int64_t
fx_round_shift(int64_t v, int shift) {
    const int64_t half = (int64_t)1 << (shift - 1);
    return v >= 0 ? (v + half) >> shift : -(((-v) + half) >> shift);
}

/* fx_round_shift() for a sum that already fits 32 bits, so a 32-bit core
 * does no 64-bit arithmetic. The caller guarantees |v| + half does not
 * overflow int32. */
static inline int32_t
fx_round_shift32(int32_t v, int shift) {
    const int32_t half = (int32_t)1 << (shift - 1);
    return v >= 0 ? (v + half) >> shift : -((-v + half) >> shift);
}

/* Multiply two fixed-point numbers in Q(*.shift) and shift the product back
 * down by `shift`, flooring toward negative infinity (see this header's top
 * comment on why that is not the same as truncating toward zero). */
static inline int32_t
fx_mul_floor(int32_t a, int32_t b, int shift) {
    return (int32_t)(((int64_t)a * (int64_t)b) >> shift);
}

/* Multiply two fixed-point numbers in Q(*.shift) and shift the product back
 * down by `shift`, rounding to the nearest representable value with ties
 * broken away from zero. */
static inline int32_t
fx_mul_round(int32_t a, int32_t b, int shift) {
    return (int32_t)fx_round_shift((int64_t)a * (int64_t)b, shift);
}

/* Widened quotient, nearest with ties away from zero. den is nonzero and
 * shift is 0..31. INT32_MIN magnitudes require widening before negation. */
static inline int64_t
fx_div_round_wide(int32_t num, int32_t den, int shift) {
    const int neg = (num < 0) != (den < 0);
    const int64_t n = (num < 0 ? -(int64_t)num : (int64_t)num) << shift;
    const int64_t d = den < 0 ? -(int64_t)den : (int64_t)den;
    const int64_t q = (n + d / 2) / d;
    return neg ? -q : q;
}

/* Narrowing preserves wrap; callers requiring saturation use fx_div_round64. */
static inline int32_t
fx_div_round(int32_t num, int32_t den, int shift) {
    return (int32_t)fx_div_round_wide(num, den, shift);
}

/* Widened division with an int32-saturated result. Zero denominators
 * saturate by the numerator's sign, with 0/0 yielding 0. shift is 0..31. */
static inline int32_t
fx_div_round64(int32_t num, int32_t den, int shift) {
    if (den == 0) {
        return num > 0 ? INT32_MAX : (num < 0 ? INT32_MIN : 0);
    }
    const int64_t q = fx_div_round_wide(num, den, shift);
    return q > INT32_MAX ? INT32_MAX : (q < INT32_MIN ? INT32_MIN : (int32_t)q);
}

/* Nearest integer, ties away from zero. den is positive; |num| + den/2
 * fits in int, so a 32-bit core needs no widened division. */
static inline int
fx_round_div(int num, int den) {
    return num >= 0 ? (num + den / 2) / den : -((-num + den / 2) / den);
}
