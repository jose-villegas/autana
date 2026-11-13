#include "draw/draw_blend.h"

#include <stdint.h>

#include "simd/simd.h"

void
draw_blend_row_scalar(gfx_color_t* dst, const rgb565_mix_toward_t* m, size_t count) {
    for (size_t i = 0; i < count; i++) {
        dst[i] = rgb565be_mix_onto(dst[i], m);
    }
}

#if !SIMD_PIE
void
draw_blend_row(gfx_color_t* dst, const rgb565_mix_toward_t* m, size_t count) {
    draw_blend_row_scalar(dst, m, count);
}
#else

/* Lanes: a Q register holds 8 pixels as 16-bit lanes, native after the byte
 * swap. Each channel is masked where it lies, never shifted down:
 *
 *   kept  = (pixel & mask) * keep >> at          the channel times 255 - t
 *   sum   = kept + add                           add = colour * t + 128
 *   out   = (sum * 257 >> (16 - at)) & mask      back where it lies
 *
 * EE.VMUL.U16 shifts its 32-bit product right by SAR and keeps the low 16
 * bits, so `at` comes off in the multiply. (sum * 257) >> 16 is
 * rgb565_div255(sum - 1) for every sum a blend makes (at most 63 * 255 + 128),
 * and shifting by 16 - at instead leaves the quotient at `at` with the
 * fraction below it, which the mask drops. Every sum stays below 0x8000, so
 * EE.VADDS.S16 never saturates. */
#define BLEND_RED_MASK       0xF800u
#define BLEND_GREEN_MASK     0x07E0u
#define BLEND_BLUE_MASK      0x001Fu
#define BLEND_RED_AT         11
#define BLEND_GREEN_AT       5
#define BLEND_BLUE_AT        0
#define BLEND_DIV_SCALE      257u /* 2^16 / 255, rounded down: (sum * 257) >> 16 */
#define BLEND_DIV_SHIFT      16
#define BLEND_DIV_ROUND      1u /* rgb565_div255()'s + 1, folded into each add */

/* The kernel's halfwords, each read by EE.VLDBC.16 into every lane, at
 * lane * BLEND_LANE_BYTES. */
#define BLEND_LANE_RED       0
#define BLEND_LANE_GREEN     1
#define BLEND_LANE_BLUE      2
#define BLEND_LANE_RED_ADD   3
#define BLEND_LANE_GREEN_ADD 4
#define BLEND_LANE_BLUE_ADD  5
#define BLEND_LANE_KEEP      6
#define BLEND_LANE_DIV       7
#define BLEND_LANES          8
#define BLEND_LANE_BYTES     2
_Static_assert(BLEND_LANE_BYTES == sizeof(uint16_t), "a lane is one halfword");

#define BLEND_STR(x)         #x
#define BLEND_XSTR(x)        BLEND_STR(x)
#define BLEND_AT(lane)       BLEND_XSTR(lane) " * " BLEND_XSTR(BLEND_LANE_BYTES)

/* `steps` of DRAW_BLEND_STEP pixels at a SIMD_ALIGN-aligned dst. */
void draw_blend_pie(gfx_color_t* dst, const uint16_t* lanes, size_t steps);

/* a2 dst, a3 lanes, a4 steps; a5 the store pointer, a8 to a13 the masks and
 * adds, reloaded into q6 and q7 per channel; q4 keep, q5 the divide. */
/* clang-format off */
__asm__(".section .text.draw_blend_pie, \"ax\", @progbits\n"
        ".align 4\n"
        ".global draw_blend_pie\n"
        ".type draw_blend_pie, @function\n"
        /* Byte order, both ways: b gets a swapped and a gets b swapped
         * (see gfx_quarter_select.c). */
        ".macro blend_swap a, b\n"
        "    ee.vunzip.8 \\a, \\b\n"
        "    ee.vzip.8 \\b, \\a\n"
        ".endm\n"
        /* One channel of `in` into `out`: mask, keep, add, divide, mask. */
        ".macro blend_channel out, in, mask, add, at\n"
        "    ee.vldbc.16 q6, \\mask\n"
        "    ee.vldbc.16 q7, \\add\n"
        "    ee.andq \\out, \\in, q6\n"
        "    ssai \\at\n"
        "    ee.vmul.u16 \\out, \\out, q4\n"
        "    ee.vadds.s16 \\out, \\out, q7\n"
        "    ssai " BLEND_XSTR(BLEND_DIV_SHIFT) " - \\at\n"
        "    ee.vmul.u16 \\out, \\out, q5\n"
        "    ee.andq \\out, \\out, q6\n"
        ".endm\n"
        /* Eight native pixels of `in` blended into `out`; `spare` is spent. */
        ".macro blend_pixels out, in, spare\n"
        "    blend_channel \\out, \\in, a8, a11, " BLEND_XSTR(BLEND_RED_AT) "\n"
        "    blend_channel \\spare, \\in, a9, a12, " BLEND_XSTR(BLEND_GREEN_AT) "\n"
        "    ee.orq \\out, \\out, \\spare\n"
        "    blend_channel \\spare, \\in, a10, a13, " BLEND_XSTR(BLEND_BLUE_AT) "\n"
        "    ee.orq \\out, \\out, \\spare\n"
        ".endm\n"
        "draw_blend_pie:\n"
        "    entry a1, 16\n" /* a leaf: nothing on the stack */
        "    addi a6, a3, " BLEND_AT(BLEND_LANE_KEEP) "\n"
        "    ee.vldbc.16 q4, a6\n"
        "    addi a6, a3, " BLEND_AT(BLEND_LANE_DIV) "\n"
        "    ee.vldbc.16 q5, a6\n"
        "    addi a8, a3, " BLEND_AT(BLEND_LANE_RED) "\n"
        "    addi a9, a3, " BLEND_AT(BLEND_LANE_GREEN) "\n"
        "    addi a10, a3, " BLEND_AT(BLEND_LANE_BLUE) "\n"
        "    addi a11, a3, " BLEND_AT(BLEND_LANE_RED_ADD) "\n"
        "    addi a12, a3, " BLEND_AT(BLEND_LANE_GREEN_ADD) "\n"
        "    addi a13, a3, " BLEND_AT(BLEND_LANE_BLUE_ADD) "\n"
        "    mov a5, a2\n"
        "    loopnez a4, 2f\n"
        "    ee.vld.128.ip q0, a2, 16\n"
        "    ee.vld.128.ip q1, a2, 16\n"
        "    blend_swap q0, q1\n" /* q1 the first eight, q0 the next, native */
        "    blend_pixels q2, q1, q3\n"
        "    blend_pixels q1, q0, q3\n"
        "    blend_swap q1, q2\n" /* q1 the first eight, q2 the next, big-endian */
        "    ee.vst.128.ip q1, a5, 16\n"
        "    ee.vst.128.ip q2, a5, 16\n"
        "2:\n"
        "    retw.n\n"
        ".size draw_blend_pie, . - draw_blend_pie\n");
/* clang-format on */

void
draw_blend_row(gfx_color_t* dst, const rgb565_mix_toward_t* m, size_t count) {
    if (count < DRAW_BLEND_MIN) {
        draw_blend_row_scalar(dst, m, count);
        return;
    }
    const size_t head = simd_head_bytes(dst) / sizeof *dst;
    draw_blend_row_scalar(dst, m, head);
    dst += head;
    count -= head;
    const uint16_t lanes[BLEND_LANES] = {
        [BLEND_LANE_RED] = BLEND_RED_MASK,
        [BLEND_LANE_GREEN] = BLEND_GREEN_MASK,
        [BLEND_LANE_BLUE] = BLEND_BLUE_MASK,
        [BLEND_LANE_RED_ADD] = (uint16_t)((m->rb >> RGB565_RB_RED_AT) + BLEND_DIV_ROUND),
        [BLEND_LANE_GREEN_ADD] = (uint16_t)(m->g + BLEND_DIV_ROUND),
        [BLEND_LANE_BLUE_ADD] = (uint16_t)((m->rb & UINT16_MAX) + BLEND_DIV_ROUND),
        [BLEND_LANE_KEEP] = (uint16_t)m->keep,
        [BLEND_LANE_DIV] = BLEND_DIV_SCALE,
    };
    const size_t steps = count / DRAW_BLEND_STEP;
    draw_blend_pie(dst, lanes, steps);
    dst += steps * DRAW_BLEND_STEP;
    count -= steps * DRAW_BLEND_STEP;
    draw_blend_row_scalar(dst, m, count);
}
#endif
