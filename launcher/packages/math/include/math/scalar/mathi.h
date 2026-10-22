/* mathi: plain integer scalars. Template sums and products wrap like any int32,
 * so a caller keeps coordinates small; a dot product widens to int64. No div,
 * sqrt or angle op, so an `i` vector has no normalize or rotation.
 *
 * `static inline`: some run in an innermost loop, where a cross-file call is
 * not free. abs and sign are prefixed, which keeps them clear of <stdlib.h>. */
#pragma once

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Rounds a byte count up to a power-of-two alignment. The padded count
 * must fit size_t. */
static inline size_t
mathi_size_ceil(size_t bytes, size_t alignment) {
    assert(alignment != 0 && (alignment & (alignment - 1)) == 0);
    assert(bytes <= SIZE_MAX - (alignment - 1));
    return (bytes + alignment - 1) & ~(alignment - 1);
}

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

static inline int
mathi_abs(int v) {
    return v < 0 ? -v : v;
}

/* -1, 0 or 1. */
static inline int
mathi_sign(int v) {
    return v > 0 ? 1 : (v < 0 ? -1 : 0);
}

static inline int
mathi_min(int a, int b) {
    return a < b ? a : b;
}

static inline int
mathi_max(int a, int b) {
    return a < b ? b : a;
}

/* |(x, y)| without a square root, to about 4%: the larger component plus two
 * fifths of the smaller. Nowhere here needs an exact length, only "how hard
 * is this being shaken" or "how far did this turn", and this is far cheaper
 * than hypot(), a libm call with a square root inside. */
static inline int
mathi_len(int x, int y) {
    const int ax = mathi_abs(x);
    const int ay = mathi_abs(y);
    const int hi = mathi_max(ax, ay);
    const int lo = mathi_min(ax, ay);

    return hi + (lo * 2) / 5;
}

/* Rounds down/up to an even coordinate: the panel controller only takes a
 * window on even edges. */
static inline int
mathi_even_floor(int v) {
    return v & ~1;
}

static inline int
mathi_even_ceil(int v) {
    return (v + 1) & ~1;
}

static inline __attribute__((always_inline)) int
mathi_clamp(int value, int lo, int hi) {
    return value < lo ? lo : (value > hi ? hi : value);
}

/* Floored modulo into [0, len); len must be positive. */
static inline int
mathi_repeat(int v, int len) {
    assert(len > 0);
    const int remainder = v % len;
    return remainder < 0 ? remainder + len : remainder;
}

/* Into [lo, hi); lo < hi. */
static inline int
mathi_wrap(int v, int lo, int hi) {
    assert(lo < hi);
    const uint32_t len = (uint32_t)hi - (uint32_t)lo;
    const uint32_t r = v >= lo ? ((uint32_t)v - (uint32_t)lo) % len : (len - ((uint32_t)lo - (uint32_t)v) % len) % len;
    return (int)((uint32_t)lo + r);
}

/* Into [0, len) for any uint32_t; len > 0. */
static inline uint32_t
mathi_repeat_u32(uint32_t v, uint32_t len) {
    assert(len > 0);
    return v % len;
}

/* Inclusive clamp over the full uint32_t range. */
static inline uint32_t
mathi_clamp_u32(uint32_t v, uint32_t lo, uint32_t hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Unsigned modulo into [0, len); len must be positive. */
static inline uint64_t
mathi_repeat_u64(uint64_t v, uint64_t len) {
    assert(len > 0);
    return v % len;
}

/* Inclusive clamp over the full uint64_t range. */
static inline uint64_t
mathi_clamp_u64(uint64_t v, uint64_t lo, uint64_t hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Floor of the square root over the whole uint64_t range. */
static inline uint32_t
mathi_isqrt64(uint64_t value) {
    enum { VALUE_BITS = 64, DIGIT_BITS = 2 };

    if (value == 0) {
        return 0;
    }
    uint64_t root = 0;
    const int shift = (VALUE_BITS - 1 - __builtin_clzll(value)) / DIGIT_BITS * DIGIT_BITS;
    uint64_t bit = UINT64_C(1) << shift;
    while (bit != 0) {
        if (value >= root + bit) {
            value -= root + bit;
            root = (root >> 1) + bit;
        } else {
            root >>= 1;
        }
        bit >>= DIGIT_BITS;
    }
    return (uint32_t)root;
}

/* half_chord = floor(sqrt(r^2-d^2)), zero for |d| >= r; |r|, |d| fit int32. */
static inline int
mathi_half_chord(int r, int d) {
    if ((int64_t)d <= -(int64_t)r || d >= r) {
        return 0;
    }
    const int64_t h = mathi_wmul(r, r) - mathi_wmul(d, d);
    return (int)mathi_isqrt64((uint64_t)h);
}

/* half_chord_round = f + (h-f^2 > f), h=r^2-d^2, f=floor(sqrt(h)); zero for |d| >= r; |r|, |d| fit int32. */
static inline int
mathi_half_chord_round(int r, int d) {
    if ((int64_t)d <= -(int64_t)r || d >= r) {
        return 0;
    }
    const int64_t h = mathi_wmul(r, r) - mathi_wmul(d, d);
    const int64_t f = mathi_isqrt64((uint64_t)h);
    return (int)(f + (h - f * f > f));
}

/* in_disc = dx^2 + dy^2 <= r^2; the squared sum must fit int64. */
static inline bool
mathi_in_disc(int dx, int dy, int r) {
    return mathi_wmul(dx, dx) + mathi_wmul(dy, dy) <= mathi_wmul(r, r);
}

/* Truncation keeps the fractional step toward a. The step is int64 because a full int32 span overflows,
 * and it divides in 32 bits when it fits: a 64-bit divide is a library call on the board. */
static inline int32_t
mathi_lerp(int32_t a, int32_t b, int32_t t, int32_t n) {
    assert(n > 0 && t >= 0 && t <= n);
    const int64_t step = ((int64_t)b - a) * t;
    if (step >= INT32_MIN && step <= INT32_MAX) {
        return a + (int32_t)step / n;
    }
    return (int32_t)((int64_t)a + step / n);
}

static inline int32_t
mathi_lerp_u8(int32_t a, int32_t b, uint8_t w) {
    return mathi_lerp(a, b, w, UINT8_MAX);
}
