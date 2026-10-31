/*
 * The copy kernels: two vector registers per step. Arguments: a2 dst
 * (SIMD_ALIGN-aligned), a3 src, a4 steps of 2 * SIMD_ALIGN bytes.
 * The _unaligned form takes any src, as the repeat kernel does.
 */
#include "simd/simd.h"

#if SIMD_PIE
#include "simd/pie_asm.h"

SIMD_PIE_FUNC simd_pie_copy
    loopgtz a4, 1f
    ee.vld.128.ip q0, a3, 16
    ee.vld.128.ip q1, a3, 16
    ee.vst.128.ip q0, a2, 16
    ee.vst.128.ip q1, a2, 16
1:
SIMD_PIE_END simd_pie_copy

SIMD_PIE_FUNC simd_pie_copy_unaligned
    ee.ld.128.usar.ip q2, a3, 16
    loopgtz a4, 1f
    ee.ld.128.usar.ip q3, a3, 16
    ee.src.q.qup q0, q2, q3
    ee.ld.128.usar.ip q3, a3, 16
    ee.src.q.qup q1, q2, q3
    ee.vst.128.ip q0, a2, 16
    ee.vst.128.ip q1, a2, 16
1:
SIMD_PIE_END simd_pie_copy_unaligned
#endif
