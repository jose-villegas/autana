/*
 * gfx_quarter_select: the blended expand's step between two half rows, eight
 * big-endian RGB565 pixels at a time. On the device it runs on the S3's PIE
 * vector unit; elsewhere, and as the device's reference, it is plain C. Both
 * give the same bytes as rgb565_spread_quarter().
 */
#pragma once
#include <stdint.h>

/* Pixels per step: a 128-bit PIE Q register of 16-bit pixels. */
#define GFX_QUARTER_LANES 8

/* Each of `steps` runs of GFX_QUARTER_LANES pixels: out = mine where the run's
 * keep byte has the lane's bit set, else three quarters of mine and one of
 * other, rounded. Every pointer but `keep` is 16-byte aligned. */
void gfx_quarter_select(const uint16_t* mine, const uint16_t* other, const uint8_t* keep, uint16_t* out, int steps);

/* The plain C body: what the host runs, and what the device's is checked against. */
void gfx_quarter_select_scalar(const uint16_t* mine, const uint16_t* other, const uint8_t* keep, uint16_t* out,
                               int steps);
