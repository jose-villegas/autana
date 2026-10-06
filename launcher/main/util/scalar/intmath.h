/*
 * intmath: small integer helpers used from more than one file.
 *
 * `static inline`: some of these are called tens of thousands of times a
 * second from an innermost loop, where a cross-file call is not free.
 */
#pragma once

#include <stdint.h>

/* Not named `abs`/`sign`: those collide with <stdlib.h>. */

static inline int
im_abs(int v) {
    return v < 0 ? -v : v;
}

/* -1, 0 or 1. */
static inline int
im_sign(int v) {
    return v > 0 ? 1 : (v < 0 ? -1 : 0);
}

static inline int
im_min(int a, int b) {
    return a < b ? a : b;
}

static inline int
im_max(int a, int b) {
    return a < b ? b : a;
}

/* |(x, y)| without a square root, to about 4%: the larger component plus two
 * fifths of the smaller. Nowhere here needs an exact length, only "how hard
 * is this being shaken" or "how far did this turn", and this is far cheaper
 * than a real hypot() on a chip with no hardware divider, let alone a square
 * root. */
static inline int
im_len(int x, int y) {
    const int ax = im_abs(x);
    const int ay = im_abs(y);
    const int hi = im_max(ax, ay);
    const int lo = im_min(ax, ay);

    return hi + (lo * 2) / 5;
}

/* Rounds down/up to an even coordinate: the panel controller only takes a
 * window on even edges. */
static inline int
even_floor(int v) {
    return v & ~1;
}

static inline int
even_ceil(int v) {
    return (v + 1) & ~1;
}

static inline __attribute__((always_inline)) int
im_clamp(int value, int lo, int hi) {
    return value < lo ? lo : (value > hi ? hi : value);
}

/* Floor of the square root, including the full unsigned 64-bit range. */
static inline uint32_t
im_isqrt64(uint64_t value) {
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
