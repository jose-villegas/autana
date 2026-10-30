/* pie_kernels: the PIE kernels' C prototypes, for the calls in src/ only. */
#pragma once

#include <stddef.h>
#include <stdint.h>

/* `blocks` of SIMD_ALIGN bytes at an aligned dst; `pair` holds the value twice. */
void simd_pie_fill_u16(uint16_t* dst, uint32_t pair, size_t blocks);

/* `blocks` of SIMD_LANES_U16 source elements, each written twice at an aligned dst. */
void simd_pie_repeat2_u16(uint16_t* dst, const uint16_t* src, size_t blocks);
void simd_pie_repeat2_u16_unaligned(uint16_t* dst, const uint16_t* src, size_t blocks);

/* `steps` of 2 * SIMD_ALIGN bytes at an aligned dst. */
void simd_pie_copy(void* dst, const void* src, size_t steps);
void simd_pie_copy_unaligned(void* dst, const void* src, size_t steps);
