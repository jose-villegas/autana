/*
 * simd: what every call and kernel shares, the vector width and whether the
 * PIE kernels are built. A call takes any element-aligned pointer: it peels
 * the unaligned head, hands the aligned body to a kernel and finishes the
 * tail itself, and below its own minimum length runs the reference alone.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __XTENSA__
#include "sdkconfig.h"
#endif

/* Bytes in one vector register: the alignment every PIE load and store needs. */
#define SIMD_ALIGN     16

/* 16-bit lanes in one vector register. */
#define SIMD_LANES_U16 (SIMD_ALIGN / (int)sizeof(uint16_t))

/* 1 where the PIE kernels are built (the ESP32-S3 with CONFIG_SIMD_PIE), else 0. */
#if defined(__XTENSA__) && defined(CONFIG_SIMD_PIE) && CONFIG_SIMD_PIE
#define SIMD_PIE 1
#else
#define SIMD_PIE 0
#endif

/* True if `p` sits on a SIMD_ALIGN boundary. */
static inline bool
simd_is_aligned(const void* p) {
    return ((uintptr_t)p & (SIMD_ALIGN - 1)) == 0;
}

/* Bytes from `p` up to the next SIMD_ALIGN boundary; 0 when it is on one. */
static inline size_t
simd_head_bytes(const void* p) {
    return (size_t)(-(uintptr_t)p & (SIMD_ALIGN - 1));
}
