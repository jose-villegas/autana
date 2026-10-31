/* fill: one value written over an array. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "simd/simd.h"

/* Rows of 16-bit elements shorter than this run the reference alone: below
 * it the head and tail cost more than the vector body saves. */
#define SIMD_FILL_U16_MIN 64

/* The C reference simd_fill_u16() matches bit for bit. */
static inline void
simd_fill_u16_ref(uint16_t* dst, uint16_t value, size_t count) {
    for (size_t i = 0; i < count; i++) {
        dst[i] = value;
    }
}

/* simd_fill_u16() at or past SIMD_FILL_U16_MIN: a scalar head to
 * SIMD_ALIGN, PIE blocks, a scalar tail. */
void simd_fill_u16_long(uint16_t* dst, uint16_t value, size_t count);

/* dst[i] = value for every i < count. Inline, so a short run costs the
 * caller no more than its own loop would. */
static inline void
simd_fill_u16(uint16_t* dst, uint16_t value, size_t count) {
    if (SIMD_PIE && count >= SIMD_FILL_U16_MIN) {
        simd_fill_u16_long(dst, value, count);
        return;
    }
    simd_fill_u16_ref(dst, value, count);
}
