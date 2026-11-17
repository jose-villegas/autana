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
#include "simd/simd.h"

/* Pixels one vector step blends: two 128-bit registers of 16-bit pixels. */
#define DRAW_BLEND_STEP 16

/* Rows shorter than this blend a pixel at a time. A row this long keeps one
 * vector step after the longest unaligned head; measured, the vector body
 * loses to the scalar one at 16 pixels and wins from 24. */
#define DRAW_BLEND_MIN  (DRAW_BLEND_STEP + (SIMD_ALIGN / (int)sizeof(gfx_color_t)) - 1)

/* dst[i] = rgb565be_mix_onto(dst[i], m) for every i < count. */
void draw_blend_row(gfx_color_t* dst, const rgb565_mix_toward_t* m, size_t count);

/* The plain C body: short rows, the host, and what the device is checked against. */
void draw_blend_row_scalar(gfx_color_t* dst, const rgb565_mix_toward_t* m, size_t count);

/* The vector body at any length, the unaligned head and the tail a pixel at
 * a time: what DRAW_BLEND_MIN is measured against. The host's is the C body. */
void draw_blend_row_vector(gfx_color_t* dst, const rgb565_mix_toward_t* m, size_t count);
