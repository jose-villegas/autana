/* copy: bytes from one array to another, memcpy's contract. */
#pragma once

#include <stddef.h>

/* Copies shorter than this many bytes run the reference alone: the board
 * crossover lies between 128 B (memcpy faster) and 192 B (PIE faster). */
#define SIMD_COPY_MIN 192

/* Copies `bytes` bytes from src to dst; the arrays do not overlap. */
void simd_copy(void* dst, const void* src, size_t bytes);

/* The C reference simd_copy() matches bit for bit (the C library's memcpy). */
void simd_copy_ref(void* dst, const void* src, size_t bytes);
