/*
 * mathi: plain integer scalar operations, for grid, pixel and cell
 * coordinates. The first group serves the math templates (math_template.h):
 * sums and products wrap like any int32, so a caller keeps its coordinates
 * small, and a dot product widens to int64. The template group has no div,
 * sqrt or angle op, so an `i` vector gets no normalize or rotation. The rest are the shared integer helpers: abs, min,
 * max, clamp, an approximate length, an integer square root, even rounding.
 *
 * `static inline`: some are called tens of thousands of times a second from
 * an innermost loop, where a cross-file call is not free. abs and sign are
 * prefixed like the rest, which also keeps them clear of <stdlib.h>.
 */
#pragma once

#include <assert.h>
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

/* Floor of the square root over the whole uint64_t range. */
static inline uint32_t
mathi_isqrt64(uint64_t value) {
    uint64_t root = 0;
    uint64_t bit = (uint64_t)1 << 62;
    while (bit > value) {
        bit >>= 2;
    }
    while (bit != 0) {
        if (value >= root + bit) {
            value -= root + bit;
            root = (root >> 1) + bit;
        } else {
            root >>= 1;
        }
        bit >>= 2;
    }
    return (uint32_t)root;
}
