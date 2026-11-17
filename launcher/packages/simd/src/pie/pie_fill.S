/*
 * The fill kernel: 16-bit lanes broadcast into one vector register and
 * stored block after block. Arguments: a2 dst (SIMD_ALIGN-aligned), a3 the
 * value in both halves of a word, a4 blocks of SIMD_ALIGN bytes.
 */
#include "simd/simd.h"

#if SIMD_PIE
#include "simd/pie_asm.h"

SIMD_PIE_FUNC simd_pie_fill_u16
    ee.movi.32.q q0, a3, 0
    ee.movi.32.q q0, a3, 1
    ee.movi.32.q q0, a3, 2
    ee.movi.32.q q0, a3, 3
    loopgtz a4, 1f
    ee.vst.128.ip q0, a2, 16
1:
SIMD_PIE_END simd_pie_fill_u16
#endif
