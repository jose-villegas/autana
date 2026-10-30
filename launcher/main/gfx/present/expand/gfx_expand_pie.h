/*
 * gfx_expand_pie, the blended expand's quarter blend on the S3's PIE vector
 * unit: eight big-endian RGB565 pixels per step, the same bytes as the scalar
 * rgb565_spread_quarter(). Device only; the host keeps the scalar path.
 */
#pragma once
#include <stdint.h>

/* Lanes per PIE step: a 128-bit Q register of 16-bit pixels. */
#define GFX_PIE_LANES 8

/* Each of `steps` runs of GFX_PIE_LANES pixels: out = mine where the run's
 * keep byte has the lane's bit set, else three quarters of mine and one of
 * other, rounded. Every pointer but `keep` is 16-byte aligned. */
void gfx_pie_quarter_select(const uint16_t* mine, const uint16_t* other, const uint8_t* keep, uint16_t* out, int steps);
