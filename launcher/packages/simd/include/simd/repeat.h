/* repeat: every element of an array written more than once, side by side. */
#pragma once

#include <stddef.h>
#include <stdint.h>

/* Source runs shorter than this run the reference alone. */
#define SIMD_REPEAT2_U16_MIN 32

/* dst[2i] = dst[2i + 1] = src[i] for every i < count: dst holds 2 * count
 * elements. The arrays do not overlap. */
void simd_repeat2_u16(uint16_t* dst, const uint16_t* src, size_t count);

/* The C reference simd_repeat2_u16() matches bit for bit. */
void simd_repeat2_u16_ref(uint16_t* dst, const uint16_t* src, size_t count);
