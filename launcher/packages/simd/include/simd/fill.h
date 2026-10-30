/* fill: one value written over an array. */
#pragma once

#include <stddef.h>
#include <stdint.h>

/* Rows of 16-bit elements shorter than this run the reference alone: below
 * it the head and tail cost more than the vector body saves. */
#define SIMD_FILL_U16_MIN 64

/* dst[i] = value for every i < count. */
void simd_fill_u16(uint16_t* dst, uint16_t value, size_t count);

/* The C reference simd_fill_u16() matches bit for bit. */
void simd_fill_u16_ref(uint16_t* dst, uint16_t value, size_t count);
