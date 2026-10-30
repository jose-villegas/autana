/*
 * The repeat-by-two kernels: eight 16-bit elements in, each zipped with
 * itself into two registers, sixteen out. Arguments: a2 dst
 * (SIMD_ALIGN-aligned), a3 src, a4 blocks of eight source elements.
 * The _unaligned form takes any 2-byte-aligned src: EE.LD.128.USAR loads the
 * aligned block holding it and EE.SRC.Q shifts the pair into place, reading
 * no block that holds none of the source.
 */
#include "simd/simd.h"

#if SIMD_PIE
#include "simd/pie_asm.h"

SIMD_PIE_FUNC simd_pie_repeat2_u16
    loopgtz a4, 1f
    ee.vld.128.ip q0, a3, 16
    ee.orq q1, q0, q0
    ee.vzip.16 q0, q1
    ee.vst.128.ip q0, a2, 16
    ee.vst.128.ip q1, a2, 16
1:
SIMD_PIE_END simd_pie_repeat2_u16

SIMD_PIE_FUNC simd_pie_repeat2_u16_unaligned
    ee.ld.128.usar.ip q2, a3, 16
    loopgtz a4, 1f
    ee.ld.128.usar.ip q3, a3, 16
    ee.src.q.qup q0, q2, q3
    ee.orq q1, q0, q0
    ee.vzip.16 q0, q1
    ee.vst.128.ip q0, a2, 16
    ee.vst.128.ip q1, a2, 16
1:
SIMD_PIE_END simd_pie_repeat2_u16_unaligned
#endif
