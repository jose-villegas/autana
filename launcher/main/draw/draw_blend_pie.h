/*
 * draw_blend_pie: what the PIE blend kernel (draw_blend_pie.S) and its
 * caller (draw_blend.c) share: the constants it blends with and where each
 * sits in the lanes the caller fills. Preprocessor only, so the assembly
 * includes it too (it reads only the shifts and lane offsets).
 */
#pragma once

/* Lanes: a Q register holds 8 pixels as 16-bit lanes, native after the byte
 * swap. Each channel is masked where it lies, never shifted down:
 *
 *   kept  = (pixel & mask) * keep >> at          the channel times 255 - t
 *   sum   = kept + add                           add = colour * t + 128
 *   out   = (sum * 257 >> (16 - at)) & mask      back where it lies
 */

/* EE.VMUL.U16 shifts its 32-bit product right by SAR and keeps the low 16
 * bits, so `at` comes off in the multiply. (sum * 257) >> 16 is
 * rgb565_div255(sum - 1) for every sum a blend makes (at most 63 * 255 + 128).
 * Shifting by 16 - at leaves the quotient at `at`, the fraction below it
 * dropped by the mask. Every sum stays below 0x8000: no saturation. */
/* Where each channel lies in a native RGB565 pixel. */
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
 * BLEND_AT(lane). */
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
#define BLEND_AT(lane)       ((lane) * BLEND_LANE_BYTES)
