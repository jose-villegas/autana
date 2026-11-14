/*
 * The blend kernel: `a4` steps of DRAW_BLEND_STEP pixels at a2
 * (SIMD_ALIGN-aligned), each blended toward one colour at one share, the
 * lanes at a3 holding what draw_blend_pie.h lays out. a5 is the store
 * pointer, a8 to a13 the masks and adds, reloaded into q6 and q7 per
 * channel; q4 keep, q5 the divide.
 */
#include "simd/simd.h"

#if SIMD_PIE
#include "draw/draw_blend_pie.h"
#include "simd/pie_asm.h"

/* clang-format off */
/* One channel of `in` into `out`: mask, keep, add, divide, mask. */
.macro blend_channel out, in, mask, add, at
    ee.vldbc.16 q6, \mask
    ee.vldbc.16 q7, \add
    ee.andq \out, \in, q6
    ssai \at
    ee.vmul.u16 \out, \out, q4
    ee.vadds.s16 \out, \out, q7
    ssai BLEND_DIV_SHIFT - \at
    ee.vmul.u16 \out, \out, q5
    ee.andq \out, \out, q6
.endm

/* Eight native pixels of `in` blended into `out`; `spare` is spent. */
.macro blend_pixels out, in, spare
    blend_channel \out, \in, a8, a11, BLEND_RED_AT
    blend_channel \spare, \in, a9, a12, BLEND_GREEN_AT
    ee.orq \out, \out, \spare
    blend_channel \spare, \in, a10, a13, BLEND_BLUE_AT
    ee.orq \out, \out, \spare
.endm

SIMD_PIE_FUNC draw_blend_pie
    addi a6, a3, BLEND_AT(BLEND_LANE_KEEP)
    ee.vldbc.16 q4, a6
    addi a6, a3, BLEND_AT(BLEND_LANE_DIV)
    ee.vldbc.16 q5, a6
    addi a8, a3, BLEND_AT(BLEND_LANE_RED)
    addi a9, a3, BLEND_AT(BLEND_LANE_GREEN)
    addi a10, a3, BLEND_AT(BLEND_LANE_BLUE)
    addi a11, a3, BLEND_AT(BLEND_LANE_RED_ADD)
    addi a12, a3, BLEND_AT(BLEND_LANE_GREEN_ADD)
    addi a13, a3, BLEND_AT(BLEND_LANE_BLUE_ADD)
    mov a5, a2
    loopnez a4, 2f
    ee.vld.128.ip q0, a2, 16
    ee.vld.128.ip q1, a2, 16
    SIMD_PIE_SWAP16 q0, q1      /* q1 the first eight, q0 the next, native */
    blend_pixels q2, q1, q3
    blend_pixels q1, q0, q3
    SIMD_PIE_SWAP16 q1, q2      /* q1 the first eight, q2 the next, big-endian */
    ee.vst.128.ip q1, a5, 16
    ee.vst.128.ip q2, a5, 16
2:
SIMD_PIE_END draw_blend_pie
/* clang-format on */
#endif
