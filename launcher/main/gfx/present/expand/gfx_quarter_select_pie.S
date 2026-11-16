/*
 * The PIE bodies of gfx_quarter_select() and gfx_quarter_spread(): eight
 * big-endian RGB565 pixels a step, the same bytes as the C bodies in
 * gfx_quarter_select.c, which the host runs.
 */
#include "simd/simd.h"

#if SIMD_PIE
#include "image/pixel/rgb565_layout.h"
#include "simd/pie_asm.h"

/* How far down red moves to sit in a lane beside blue, the rounding a
 * quarter adds before its divide by four. */
#define QUARTER_RED_DOWN 3
#define QUARTER_ROUND    2

/* clang-format off */
/* The PIE conventions both kernels follow sit beside their first use in
 * gfx_quarter_select: byte order at its loads, the saturation guard at its
 * sums, the shifts at its first shift, the per-lane choice at its select. */
.section .rodata.gfx_pie_constants, "a"
.align 16
gfx_pie_lane_bits: .short 1, 2, 4, 8, 16, 32, 64, 128
gfx_pie_red_lane: .short RGB565_RED_MASK >> QUARTER_RED_DOWN /* red, in its lane beside blue */
gfx_pie_blue: .short RGB565_BLUE_MASK
gfx_pie_rb_round: .short (QUARTER_ROUND << (RGB565_RED_AT - QUARTER_RED_DOWN)) | (QUARTER_ROUND << RGB565_BLUE_AT)
gfx_pie_green: .short RGB565_GREEN_MASK
gfx_pie_green_round: .short QUARTER_ROUND << RGB565_GREEN_AT
gfx_pie_red_out: .short RGB565_RED_MASK
/* blend becomes mine where mask is set (Per-lane choice, below). */
.macro pie_pick blend, mine, mask, spare
    ee.xorq \spare, \blend, \mine
    ee.andq \spare, \spare, \mask
    ee.xorq \blend, \blend, \spare
.endm
/* gfx_quarter_spread's side: other (native, its green read) blended into rb
 * against q3 and q2, 3 mine + 2 of green and of red-blue. Takes SAR 3 and
 * the red lane in q4; leaves the blend in rb and q4 spent. */
.macro pie_spread_side other, rb, spare
    ee.vsr.32 \rb, \other
    ee.andq \rb, \rb, q4
    ee.andq q4, \other, q6
    ee.andq \other, \other, q7
    ee.orq \rb, \rb, q4
    ee.vadds.s16 \other, \other, q3
    ee.vadds.s16 \rb, \rb, q2
    ssai 2
    ee.vsr.32 \other, \other
    ee.vsr.32 \spare, \rb
    ee.andq \other, \other, q7
    ee.andq \spare, \spare, q6
    ssai 1
    ee.vsl.32 \rb, \rb
    ee.vldbc.16 q4, a11
    ee.orq \other, \other, \spare
    ee.andq \rb, \rb, q4
    ee.orq \rb, \rb, \other
.endm

/* Pointers to the constants are set once, before a zero-overhead loop; blue
 * and green stay in q6 and q7, the rest are broadcast each step. The green
 * sum runs between the red-blue steps, so neither waits on its own last
 * result. a2 mine, a3 other, a4 keep, a5 out, a6 steps; a7 to a12
 * constants. */
SIMD_PIE_FUNC gfx_quarter_select
    movi a7, gfx_pie_red_lane
    movi a8, gfx_pie_rb_round
    movi a9, gfx_pie_green_round
    movi a10, gfx_pie_red_out
    movi a11, gfx_pie_lane_bits
    movi a12, gfx_pie_blue
    ee.vldbc.16 q6, a12
    movi a12, gfx_pie_green
    ee.vldbc.16 q7, a12
    loopnez a6, 2f
/* Lanes: a Q register holds 8 pixels as 16-bit lanes. Memory is big-endian
 * RGB565, so a load swaps each lane's bytes (EE.VUNZIP.8 then EE.VZIP.8 on
 * a register pair, which also swaps the pair) and a store swaps them back. */
    ee.vld.128.ip q0, a2, 16
    ee.vld.128.ip q1, a3, 16
    SIMD_PIE_SWAP16 q0, q1 /* q1 mine, q0 other, native */
/* Saturation guard: EE.VADDS.S16 saturates at 0x7FFF, so every sum is laid
 * out to stay below 0x8000. Red and blue share a lane (blue in bits 0..6,
 * red from bit 8, 4 x 0x1F00 + rounding = 0x7E00) and green has its own
 * (4 x 0x07E0 + rounding = 0x1FC0). */
    ee.andq q2, q1, q7
    ee.andq q3, q0, q7
    ee.vldbc.16 q4, a7 /* red in its lane */
/* Shifts: EE.VSR.32 and EE.VSL.32 shift 32-bit lanes by SAR, so bits cross
 * between a pair of 16-bit lanes; every shift is followed by a mask that
 * drops them. EE.VSR.32 is arithmetic, and the mask drops the sign fill
 * too. */
    ssai QUARTER_RED_DOWN
    ee.vsr.32 q5, q1
    ee.vadds.s16 q3, q3, q2
    ee.andq q5, q5, q4
    ee.vadds.s16 q3, q3, q2
    ee.vadds.s16 q3, q3, q2 /* 3 mine + other, green */
    ee.andq q2, q1, q6
    ee.orq q5, q5, q2 /* mine red-blue */
    ee.vsr.32 q2, q0
    ee.andq q2, q2, q4
    ee.andq q4, q0, q6
    ee.orq q2, q2, q4 /* other red-blue */
    ee.vldbc.16 q0, a9
    ee.vadds.s16 q2, q2, q5
    ee.vadds.s16 q3, q3, q0 /* + 2, green */
    ee.vadds.s16 q2, q2, q5
    ee.vldbc.16 q0, a8
    ee.vadds.s16 q2, q2, q5
    ssai 2
    ee.vsr.32 q3, q3
    ee.vadds.s16 q2, q2, q0 /* 3 mine + other + 2, red-blue */
    ee.andq q3, q3, q7 /* green out */
    ee.vsr.32 q5, q2
    ssai 1
    ee.vsl.32 q2, q2
    ee.vldbc.16 q0, a10
    ee.andq q5, q5, q6 /* blue out */
    ee.andq q2, q2, q0 /* red out */
/* Per-lane choice: a mask from the step's keep byte (EE.VLDBC.8 puts it in
 * both halves of every lane; AND with 1, 2, 4 ... 128 keeps the low copy's
 * bit, compare equal), then out = a ^ ((a ^ b) & mask). */
    ee.vldbc.8.ip q4, a4, 1 /* the keep byte, in every byte */
    ee.vld.128.ip q0, a11, 0
    ee.orq q2, q2, q5
    ee.andq q4, q4, q0
    ee.orq q2, q2, q3 /* the blend, native */
    ee.vcmp.eq.s16 q4, q4, q0 /* all ones where kept */
    pie_pick q2, q1, q4, q0
    SIMD_PIE_SWAP16 q2, q3 /* q3 big-endian */
    ee.vst.128.ip q3, a5, 16
2:
SIMD_PIE_END gfx_quarter_select

/* gfx_quarter_spread works out 3 mine + rounding once for both sides, blends
 * the left, then the right, swaps the pair back to big-endian at once, selects
 * each against mine, and EE.VZIP.16 lays the two out as the panel row: a2
 * mine, a3 left, a4 right, a5 keeps, a6 out, a7 steps; a8 to a14 constants
 * and the right keeps. */
SIMD_PIE_FUNC gfx_quarter_spread
    movi a8, gfx_pie_red_lane
    movi a9, gfx_pie_rb_round
    movi a10, gfx_pie_green_round
    movi a11, gfx_pie_red_out
    movi a12, gfx_pie_lane_bits
    movi a13, gfx_pie_blue
    ee.vldbc.16 q6, a13
    movi a13, gfx_pie_green
    ee.vldbc.16 q7, a13
    add a14, a5, a7 /* the right keeps */
    loopnez a7, 2f
    ee.vld.128.ip q0, a2, 0 /* mine, read again for the select */
    ee.vld.128.ip q1, a3, 16
    SIMD_PIE_SWAP16 q0, q1 /* q1 mine, q0 left, native */
    ee.vldbc.16 q4, a8
    ssai QUARTER_RED_DOWN
    ee.vsr.32 q5, q1
    ee.andq q2, q1, q7
    ee.andq q5, q5, q4
    ee.andq q1, q1, q6
    ee.vadds.s16 q3, q2, q2
    ee.orq q5, q5, q1 /* mine red-blue */
    ee.vadds.s16 q3, q3, q2
    ee.vadds.s16 q2, q5, q5
    ee.vldbc.16 q1, a10
    ee.vadds.s16 q2, q2, q5
    ee.vadds.s16 q3, q3, q1 /* 3 mine + 2, green: both sides' */
    ee.vldbc.16 q1, a9
    ee.vadds.s16 q2, q2, q1 /* 3 mine + 2, red-blue: both sides' */
    pie_spread_side q0, q5, q1 /* the left blend, native */
    ee.vld.128.ip q0, a4, 16
    SIMD_PIE_SWAP16 q0, q1 /* q1 right, native; q0 spent */
    ee.vldbc.16 q4, a8
    ssai QUARTER_RED_DOWN
    pie_spread_side q1, q0, q2 /* the right blend, native */
    SIMD_PIE_SWAP16 q5, q0 /* q0 left, q5 right, big-endian */
    ee.vld.128.ip q1, a2, 16
    ee.vld.128.ip q4, a12, 0
    ee.vldbc.8.ip q2, a5, 1
    ee.vldbc.8.ip q3, a14, 1
    ee.andq q2, q2, q4
    ee.andq q3, q3, q4
    ee.vcmp.eq.s16 q2, q2, q4
    ee.vcmp.eq.s16 q3, q3, q4
    pie_pick q0, q1, q2, q4
    pie_pick q5, q1, q3, q4
    ee.vzip.16 q0, q5 /* q0 the first four pixel pairs, q5 the last four */
    ee.vst.128.ip q0, a6, 16
    ee.vst.128.ip q5, a6, 16
2:
SIMD_PIE_END gfx_quarter_spread
/* clang-format on */
#endif
