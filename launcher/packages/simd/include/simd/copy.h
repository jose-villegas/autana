/* copy: bytes from one array to another, memcpy's contract. */
#pragma once

#include <stddef.h>
#include <string.h>

#include "simd/simd.h"

/* Copies shorter than this many bytes run the reference alone: the board
 * crossover lies between 128 B (memcpy faster) and 192 B (PIE faster). */
#define SIMD_COPY_MIN 192

/* The C reference simd_copy() matches bit for bit (the C library's memcpy). */
static inline void
simd_copy_ref(void* dst, const void* src, size_t bytes) {
    memcpy(dst, src, bytes);
}

/* simd_copy() at or past SIMD_COPY_MIN: memcpy up to SIMD_ALIGN on dst, PIE
 * steps, memcpy for the tail. */
void simd_copy_long(void* dst, const void* src, size_t bytes);

/* Copies `bytes` bytes from src to dst; the arrays do not overlap. Inline,
 * so a short copy is the caller's own memcpy. */
static inline void
simd_copy(void* dst, const void* src, size_t bytes) {
    if (SIMD_PIE && bytes >= SIMD_COPY_MIN) {
        simd_copy_long(dst, src, bytes);
        return;
    }
    simd_copy_ref(dst, src, bytes);
}
