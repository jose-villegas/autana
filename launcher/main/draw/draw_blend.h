/*
 * draw_blend: a row of pixels blended toward one colour at one share, the
 * flat inside of a translucent rectangle. On the device a long row runs on
 * the S3's PIE vector unit, sixteen pixels a step; elsewhere, and as the
 * device's reference, it is rgb565be_mix_onto() a pixel at a time. Both give
 * the same bytes.
 */
#pragma once

#include <stddef.h>

#include "gfx/target/gfx_color.h"
#include "image/pixel/rgb565.h"

/* Pixels one vector step blends: two 128-bit registers of 16-bit pixels. */
#define DRAW_BLEND_STEP 16

/* Rows shorter than this blend a pixel at a time: below it the unaligned head
 * and the tail cost more than the vector steps save. */
#define DRAW_BLEND_MIN  (4 * DRAW_BLEND_STEP)

/* dst[i] = rgb565be_mix_onto(dst[i], m) for every i < count. */
void draw_blend_row(gfx_color_t* dst, const rgb565_mix_toward_t* m, size_t count);

/* The plain C body: short rows, the host, and what the device is checked against. */
void draw_blend_row_scalar(gfx_color_t* dst, const rgb565_mix_toward_t* m, size_t count);
