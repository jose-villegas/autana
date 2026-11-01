#include "gfx/present/expand/gfx_quarter_select.h"

#include <stdbool.h>

#include "image/pixel/rgb565.h"

/* Pixel x's pick: mine where its bit in `keep` is set, else the quarter step. */
static uint16_t
quarter_or_kept(uint16_t mine, uint16_t other, const uint8_t* keep, int x) {
    const bool kept = (keep[x / GFX_QUARTER_LANES] >> (x % GFX_QUARTER_LANES)) & 1U;
    return kept ? mine : rgb565be_unspread(rgb565_spread_quarter(rgb565be_spread(mine), rgb565be_spread(other)));
}

void
gfx_quarter_select_scalar(const uint16_t* mine, const uint16_t* other, const uint8_t* keep, uint16_t* out, int steps) {
    for (int x = 0; x < steps * GFX_QUARTER_LANES; x++) {
        out[x] = quarter_or_kept(mine[x], other[x], keep, x);
    }
}

void
gfx_quarter_spread_scalar(const uint16_t* mine, const uint16_t* left, const uint16_t* right, const uint8_t* keeps,
                          uint16_t* out, int steps) {
    for (int x = 0; x < steps * GFX_QUARTER_LANES; x++) {
        out[2 * x] = quarter_or_kept(mine[x], left[x], keeps, x);
        out[(2 * x) + 1] = quarter_or_kept(mine[x], right[x], keeps + steps, x);
    }
}

#ifndef ESP_PLATFORM
void
gfx_quarter_select(const uint16_t* mine, const uint16_t* other, const uint8_t* keep, uint16_t* out, int steps) {
    gfx_quarter_select_scalar(mine, other, keep, out, steps);
}

void
gfx_quarter_spread(const uint16_t* mine, const uint16_t* left, const uint16_t* right, const uint8_t* keeps,
                   uint16_t* out, int steps) {
    gfx_quarter_spread_scalar(mine, left, right, keeps, out, steps);
}
#else

/* The PIE conventions both kernels follow sit beside their first use in
 * gfx_quarter_select: byte order at its loads, the saturation guard at its
 * sums, the shifts at its first shift, the per-lane choice at its select. */
__asm__(".section .rodata.gfx_pie_constants, \"a\"\n"
        ".align 16\n"
        "gfx_pie_lane_bits: .short 1, 2, 4, 8, 16, 32, 64, 128\n"
        "gfx_pie_red_lane: .short 0x1F00\n" /* red, shifted into its lane beside blue */
        "gfx_pie_blue: .short 0x001F\n"
        "gfx_pie_rb_round: .short 0x0202\n"
        "gfx_pie_green: .short 0x07E0\n"
        "gfx_pie_green_round: .short 0x0040\n"
        "gfx_pie_red_out: .short 0xF800\n"
        /* Byte order, both ways: b gets a swapped and a gets b swapped (Lanes, below). */
        ".macro pie_swap a, b\n"
        "    ee.vunzip.8 \\a, \\b\n"
        "    ee.vzip.8 \\b, \\a\n"
        ".endm\n"
        /* blend becomes mine where mask is set (Per-lane choice, below). */
        ".macro pie_pick blend, mine, mask, spare\n"
        "    ee.xorq \\spare, \\blend, \\mine\n"
        "    ee.andq \\spare, \\spare, \\mask\n"
        "    ee.xorq \\blend, \\blend, \\spare\n"
        ".endm\n"
        /* gfx_quarter_spread's side: other (native, its green read) blended into rb
         * against q3 and q2, 3 mine + 2 of green and of red-blue. Takes SAR 3 and
         * the red lane in q4; leaves the blend in rb and q4 spent. */
        ".macro pie_spread_side other, rb, spare\n"
        "    ee.vsr.32 \\rb, \\other\n"
        "    ee.andq \\rb, \\rb, q4\n"
        "    ee.andq q4, \\other, q6\n"
        "    ee.andq \\other, \\other, q7\n"
        "    ee.orq \\rb, \\rb, q4\n"
        "    ee.vadds.s16 \\other, \\other, q3\n"
        "    ee.vadds.s16 \\rb, \\rb, q2\n"
        "    ssai 2\n"
        "    ee.vsr.32 \\other, \\other\n"
        "    ee.vsr.32 \\spare, \\rb\n"
        "    ee.andq \\other, \\other, q7\n"
        "    ee.andq \\spare, \\spare, q6\n"
        "    ssai 1\n"
        "    ee.vsl.32 \\rb, \\rb\n"
        "    ee.vldbc.16 q4, a11\n"
        "    ee.orq \\other, \\other, \\spare\n"
        "    ee.andq \\rb, \\rb, q4\n"
        "    ee.orq \\rb, \\rb, \\other\n"
        ".endm\n");

__asm__(".section .text.gfx_quarter_select, \"ax\", @progbits\n"
        ".align 4\n"
        ".global gfx_quarter_select\n"
        ".type gfx_quarter_select, @function\n"
        /* Pointers to the constants are set once, before a zero-overhead loop; blue
         * and green stay in q6 and q7, the rest are broadcast each step. The green
         * sum runs between the red-blue steps, so neither waits on its own last
         * result. a2 mine, a3 other, a4 keep, a5 out, a6 steps; a7 to a12
         * constants. */
        "gfx_quarter_select:\n"
        "    entry a1, 16\n" /* a leaf: nothing on the stack */
        "    movi a7, gfx_pie_red_lane\n"
        "    movi a8, gfx_pie_rb_round\n"
        "    movi a9, gfx_pie_green_round\n"
        "    movi a10, gfx_pie_red_out\n"
        "    movi a11, gfx_pie_lane_bits\n"
        "    movi a12, gfx_pie_blue\n"
        "    ee.vldbc.16 q6, a12\n"
        "    movi a12, gfx_pie_green\n"
        "    ee.vldbc.16 q7, a12\n"
        "    loopnez a6, 2f\n"
        /* Lanes: a Q register holds 8 pixels as 16-bit lanes. Memory is big-endian
         * RGB565, so a load swaps each lane's bytes (EE.VUNZIP.8 then EE.VZIP.8 on
         * a register pair, which also swaps the pair) and a store swaps them back. */
        "    ee.vld.128.ip q0, a2, 16\n"
        "    ee.vld.128.ip q1, a3, 16\n"
        "    pie_swap q0, q1\n" /* q1 mine, q0 other, native */
        /* Saturation guard: EE.VADDS.S16 saturates at 0x7FFF, so every sum is laid
         * out to stay below 0x8000. Red and blue share a lane (blue in bits 0..6,
         * red from bit 8, 4 x 0x1F00 + rounding = 0x7E00) and green has its own
         * (4 x 0x07E0 + rounding = 0x1FC0). */
        "    ee.andq q2, q1, q7\n"
        "    ee.andq q3, q0, q7\n"
        "    ee.vldbc.16 q4, a7\n" /* red in its lane */
        /* Shifts: EE.VSR.32 and EE.VSL.32 shift 32-bit lanes by SAR, so bits cross
         * between a pair of 16-bit lanes; every shift is followed by a mask that
         * drops them. EE.VSR.32 is arithmetic, and the mask drops the sign fill
         * too. */
        "    ssai 3\n"
        "    ee.vsr.32 q5, q1\n"
        "    ee.vadds.s16 q3, q3, q2\n"
        "    ee.andq q5, q5, q4\n"
        "    ee.vadds.s16 q3, q3, q2\n"
        "    ee.vadds.s16 q3, q3, q2\n" /* 3 mine + other, green */
        "    ee.andq q2, q1, q6\n"
        "    ee.orq q5, q5, q2\n" /* mine red-blue */
        "    ee.vsr.32 q2, q0\n"
        "    ee.andq q2, q2, q4\n"
        "    ee.andq q4, q0, q6\n"
        "    ee.orq q2, q2, q4\n" /* other red-blue */
        "    ee.vldbc.16 q0, a9\n"
        "    ee.vadds.s16 q2, q2, q5\n"
        "    ee.vadds.s16 q3, q3, q0\n" /* + 2, green */
        "    ee.vadds.s16 q2, q2, q5\n"
        "    ee.vldbc.16 q0, a8\n"
        "    ee.vadds.s16 q2, q2, q5\n"
        "    ssai 2\n"
        "    ee.vsr.32 q3, q3\n"
        "    ee.vadds.s16 q2, q2, q0\n" /* 3 mine + other + 2, red-blue */
        "    ee.andq q3, q3, q7\n"      /* green out */
        "    ee.vsr.32 q5, q2\n"
        "    ssai 1\n"
        "    ee.vsl.32 q2, q2\n"
        "    ee.vldbc.16 q0, a10\n"
        "    ee.andq q5, q5, q6\n" /* blue out */
        "    ee.andq q2, q2, q0\n" /* red out */
        /* Per-lane choice: a mask from the step's keep byte (EE.VLDBC.8 puts it in
         * both halves of every lane; AND with 1, 2, 4 ... 128 keeps the low copy's
         * bit, compare equal), then out = a ^ ((a ^ b) & mask). */
        "    ee.vldbc.8.ip q4, a4, 1\n" /* the keep byte, in every byte */
        "    ee.vld.128.ip q0, a11, 0\n"
        "    ee.orq q2, q2, q5\n"
        "    ee.andq q4, q4, q0\n"
        "    ee.orq q2, q2, q3\n"         /* the blend, native */
        "    ee.vcmp.eq.s16 q4, q4, q0\n" /* all ones where kept */
        "    pie_pick q2, q1, q4, q0\n"
        "    pie_swap q2, q3\n" /* q3 big-endian */
        "    ee.vst.128.ip q3, a5, 16\n"
        "2:\n"
        "    retw.n\n"
        ".size gfx_quarter_select, . - gfx_quarter_select\n");

/* gfx_quarter_spread works out 3 mine + rounding once for both sides, blends
 * the left, then the right, swaps the pair back to big-endian at once, selects
 * each against mine, and EE.VZIP.16 lays the two out as the panel row: a2
 * mine, a3 left, a4 right, a5 keeps, a6 out, a7 steps; a8 to a14 constants
 * and the right keeps. */
__asm__(".section .text.gfx_quarter_spread, \"ax\", @progbits\n"
        ".global gfx_quarter_spread\n"
        ".type gfx_quarter_spread, @function\n"
        "gfx_quarter_spread:\n"
        "    entry a1, 16\n"
        "    movi a8, gfx_pie_red_lane\n"
        "    movi a9, gfx_pie_rb_round\n"
        "    movi a10, gfx_pie_green_round\n"
        "    movi a11, gfx_pie_red_out\n"
        "    movi a12, gfx_pie_lane_bits\n"
        "    movi a13, gfx_pie_blue\n"
        "    ee.vldbc.16 q6, a13\n"
        "    movi a13, gfx_pie_green\n"
        "    ee.vldbc.16 q7, a13\n"
        "    add a14, a5, a7\n" /* the right keeps */
        "    loopnez a7, 2f\n"
        "    ee.vld.128.ip q0, a2, 0\n" /* mine, read again for the select */
        "    ee.vld.128.ip q1, a3, 16\n"
        "    pie_swap q0, q1\n" /* q1 mine, q0 left, native */
        "    ee.vldbc.16 q4, a8\n"
        "    ssai 3\n"
        "    ee.vsr.32 q5, q1\n"
        "    ee.andq q2, q1, q7\n"
        "    ee.andq q5, q5, q4\n"
        "    ee.andq q1, q1, q6\n"
        "    ee.vadds.s16 q3, q2, q2\n"
        "    ee.orq q5, q5, q1\n" /* mine red-blue */
        "    ee.vadds.s16 q3, q3, q2\n"
        "    ee.vadds.s16 q2, q5, q5\n"
        "    ee.vldbc.16 q1, a10\n"
        "    ee.vadds.s16 q2, q2, q5\n"
        "    ee.vadds.s16 q3, q3, q1\n" /* 3 mine + 2, green: both sides' */
        "    ee.vldbc.16 q1, a9\n"
        "    ee.vadds.s16 q2, q2, q1\n"    /* 3 mine + 2, red-blue: both sides' */
        "    pie_spread_side q0, q5, q1\n" /* the left blend, native */
        "    ee.vld.128.ip q0, a4, 16\n"
        "    pie_swap q0, q1\n" /* q1 right, native; q0 spent */
        "    ee.vldbc.16 q4, a8\n"
        "    ssai 3\n"
        "    pie_spread_side q1, q0, q2\n" /* the right blend, native */
        "    pie_swap q5, q0\n"            /* q0 left, q5 right, big-endian */
        "    ee.vld.128.ip q1, a2, 16\n"
        "    ee.vld.128.ip q4, a12, 0\n"
        "    ee.vldbc.8.ip q2, a5, 1\n"
        "    ee.vldbc.8.ip q3, a14, 1\n"
        "    ee.andq q2, q2, q4\n"
        "    ee.andq q3, q3, q4\n"
        "    ee.vcmp.eq.s16 q2, q2, q4\n"
        "    ee.vcmp.eq.s16 q3, q3, q4\n"
        "    pie_pick q0, q1, q2, q4\n"
        "    pie_pick q5, q1, q3, q4\n"
        "    ee.vzip.16 q0, q5\n" /* q0 the first four pixel pairs, q5 the last four */
        "    ee.vst.128.ip q0, a6, 16\n"
        "    ee.vst.128.ip q5, a6, 16\n"
        "2:\n"
        "    retw.n\n"
        ".size gfx_quarter_spread, . - gfx_quarter_spread\n");

#endif
