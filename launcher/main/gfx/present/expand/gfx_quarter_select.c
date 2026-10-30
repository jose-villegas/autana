#include "gfx/present/expand/gfx_quarter_select.h"

#include <stdbool.h>

#include "image/pixel/rgb565.h"

void
gfx_quarter_select_scalar(const uint16_t* mine, const uint16_t* other, const uint8_t* keep, uint16_t* out, int steps) {
    for (int x = 0; x < steps * GFX_QUARTER_LANES; x++) {
        const bool kept = (keep[x / GFX_QUARTER_LANES] >> (x % GFX_QUARTER_LANES)) & 1U;
        out[x] = kept ? mine[x]
                      : rgb565be_unspread(rgb565_spread_quarter(rgb565be_spread(mine[x]), rgb565be_spread(other[x])));
    }
}

#ifndef ESP_PLATFORM
void
gfx_quarter_select(const uint16_t* mine, const uint16_t* other, const uint8_t* keep, uint16_t* out, int steps) {
    gfx_quarter_select_scalar(mine, other, keep, out, steps);
}
#else

/*
 * The PIE conventions this file sets:
 *
 * - Lanes: a Q register holds 8 pixels as 16-bit lanes. Memory is big-endian
 *   RGB565, so a load swaps each lane's bytes (EE.VUNZIP.8 then EE.VZIP.8 on a
 *   register pair, which also swaps the pair) and a store swaps them back.
 * - Saturation guard: EE.VADDS.S16 saturates at 0x7FFF, so every sum is laid
 *   out to stay below 0x8000. Here red and blue share a lane (blue in bits
 *   0..6, red from bit 8, 4 x 0x1F00 + rounding = 0x7E00) and green has its
 *   own (4 x 0x07E0 + rounding = 0x1FC0).
 * - Shifts: EE.VSR.32 and EE.VSL.32 shift 32-bit lanes by SAR, so bits cross
 *   between a pair of 16-bit lanes; every shift is followed by a mask that
 *   drops them. EE.VSR.32 is arithmetic, and the mask drops the sign fill too.
 * - Per-lane choice: a mask from the step's keep byte (EE.VLDBC.8 puts it in
 *   both halves of every lane; AND with 1, 2, 4 ... 128 keeps the low copy's
 *   bit, compare equal), then out = a ^ ((a ^ b) & mask).
 *
 * a2 mine, a3 other, a4 keep, a5 out, a6 steps; a7 and a8 constants.
 */
__asm__(".section .rodata.gfx_pie_constants, \"a\"\n"
        ".align 16\n"
        "gfx_pie_lane_bits: .short 1, 2, 4, 8, 16, 32, 64, 128\n"
        /* red in its lane, blue, red-blue rounding, green, green rounding, red out */
        "gfx_pie_masks: .short 0x1F00, 0x001F, 0x0202, 0x07E0, 0x0040, 0xF800\n"
        ".text\n"
        ".align 4\n"
        ".global gfx_quarter_select\n"
        ".type gfx_quarter_select, @function\n"
        "gfx_quarter_select:\n"
        "    entry a1, 16\n" /* a leaf: nothing on the stack */
        "    beqz a6, 2f\n"
        "1:\n"
        "    ee.vld.128.ip q0, a2, 16\n"
        "    ee.vld.128.ip q1, a3, 16\n"
        "    ee.vunzip.8 q0, q1\n"
        "    ee.vzip.8 q1, q0\n" /* q1 mine, q0 other, native */
        "    movi a7, gfx_pie_masks\n"
        "    ee.vldbc.16.ip q7, a7, 2\n" /* red in its lane */
        "    ee.vldbc.16.ip q6, a7, 2\n" /* blue */
        "    ssai 3\n"
        "    ee.vsr.32 q2, q1\n"
        "    ee.andq q2, q2, q7\n"
        "    ee.andq q3, q1, q6\n"
        "    ee.orq q2, q2, q3\n" /* mine red-blue */
        "    ee.vsr.32 q3, q0\n"
        "    ee.andq q3, q3, q7\n"
        "    ee.andq q4, q0, q6\n"
        "    ee.orq q3, q3, q4\n" /* other red-blue */
        "    ee.vadds.s16 q4, q2, q2\n"
        "    ee.vadds.s16 q4, q4, q2\n"
        "    ee.vadds.s16 q4, q4, q3\n"
        "    ee.vldbc.16.ip q7, a7, 2\n" /* red-blue rounding */
        "    ee.vadds.s16 q4, q4, q7\n"  /* 3 mine + other + 2, red-blue */
        "    ee.vldbc.16.ip q7, a7, 2\n" /* green */
        "    ee.andq q2, q1, q7\n"
        "    ee.andq q3, q0, q7\n"
        "    ee.vadds.s16 q5, q2, q2\n"
        "    ee.vadds.s16 q5, q5, q2\n"
        "    ee.vadds.s16 q5, q5, q3\n"
        "    ee.vldbc.16.ip q3, a7, 2\n" /* green rounding */
        "    ee.vadds.s16 q5, q5, q3\n"  /* 3 mine + other + 2, green */
        "    ssai 2\n"
        "    ee.vsr.32 q5, q5\n"
        "    ee.andq q5, q5, q7\n" /* green out */
        "    ee.vsr.32 q2, q4\n"
        "    ee.andq q2, q2, q6\n" /* blue out */
        "    ssai 1\n"
        "    ee.vsl.32 q3, q4\n"
        "    ee.vldbc.16 q7, a7\n" /* red out */
        "    ee.andq q3, q3, q7\n"
        "    ee.orq q2, q2, q3\n"
        "    ee.orq q2, q2, q5\n"       /* the blend, native */
        "    ee.vldbc.8.ip q3, a4, 1\n" /* the keep byte, in every byte */
        "    movi a8, gfx_pie_lane_bits\n"
        "    ee.vld.128.ip q4, a8, 0\n"
        "    ee.andq q3, q3, q4\n"
        "    ee.vcmp.eq.s16 q3, q3, q4\n" /* all ones where kept */
        "    ee.xorq q4, q2, q1\n"
        "    ee.andq q4, q4, q3\n"
        "    ee.xorq q2, q2, q4\n"
        "    ee.vunzip.8 q2, q3\n"
        "    ee.vzip.8 q3, q2\n" /* q3 big-endian */
        "    ee.vst.128.ip q3, a5, 16\n"
        "    addi a6, a6, -1\n"
        "    bnez a6, 1b\n"
        "2:\n"
        "    retw.n\n"
        ".size gfx_quarter_select, . - gfx_quarter_select\n");

#endif
