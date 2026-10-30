/* repeat: every element of an array written more than once, side by side. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "simd/simd.h"

/* Source runs shorter than this run the reference alone. */
#define SIMD_REPEAT2_U16_MIN 32

/* The C reference simd_repeat2_u16() matches bit for bit. */
static inline void
simd_repeat2_u16_ref(uint16_t* dst, const uint16_t* src, size_t count) {
    for (size_t i = 0; i < count; i++) {
        dst[2 * i] = src[i];
        dst[(2 * i) + 1] = src[i];
    }
}

/* simd_repeat2_u16() at or past SIMD_REPEAT2_U16_MIN: a scalar head to
 * SIMD_ALIGN, PIE blocks, a scalar tail. */
void simd_repeat2_u16_long(uint16_t* dst, const uint16_t* src, size_t count);

/* dst[2i] = dst[2i + 1] = src[i] for every i < count: dst holds 2 * count
 * elements. The arrays do not overlap. Inline, so a short run costs the
 * caller no more than its own loop would. */
static inline void
simd_repeat2_u16(uint16_t* dst, const uint16_t* src, size_t count) {
    if (SIMD_PIE && count >= SIMD_REPEAT2_U16_MIN) {
        simd_repeat2_u16_long(dst, src, count);
        return;
    }
    simd_repeat2_u16_ref(dst, src, count);
}
