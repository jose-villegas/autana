/*
 * fixed: shift-based fixed-point arithmetic at caller-selected scales.
 *
 * Products widen to 64 bits before the shift because multiplying two 32-bit
 * fixed-point numbers in 32 bits overflows before the shift.
 * fx_mul_floor and fx_mul_round are not interchangeable: on a signed, often-
 * negative accumulator rounding moves each negative step up and the error
 * compounds, so keep whichever an accumulator already uses and round only
 * one-shot values.
 * Header-only helpers keep inner loops free of cross-file calls.
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
 * down by `shift`; an arithmetic shift of a negative value floors toward
 * negative infinity, unlike a truncating cast. */
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

/* den must be nonzero. The result wraps if it overflows int32; mathx_div saturates instead. */
static inline int32_t
fx_div_round(int32_t num, int32_t den, int shift) {
    return (int32_t)fx_div_round_wide(num, den, shift);
}
