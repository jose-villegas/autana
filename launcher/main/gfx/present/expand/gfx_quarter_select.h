/*
 * gfx_quarter_select: the blended expand's quarter step toward a neighbour,
 * down or across, eight big-endian RGB565 pixels at a time; gfx_quarter_spread
 * takes both steps across at once and writes the panel row they make. On the device it runs on the S3's PIE
 * vector unit; elsewhere, and as the device's reference, it is plain C. Both
 * give the same bytes as rgb565_spread_quarter().
 */
#pragma once
#include <stdint.h>

/* Pixels per step: a 128-bit PIE Q register of 16-bit pixels. */
#define GFX_QUARTER_LANES 8

/* A vector load's alignment: where every row but the keep bytes starts. */
#define GFX_QUARTER_ALIGN 16

/* Each of `steps` runs of GFX_QUARTER_LANES pixels: out = mine where the run's
 * keep byte has the lane's bit set, else three quarters of mine and one of
 * other, rounded. Every pointer but `keep` is at GFX_QUARTER_ALIGN. */
void gfx_quarter_select(const uint16_t* mine, const uint16_t* other, const uint8_t* keep, uint16_t* out, int steps);

/* The plain C body: what the host runs, and what the device's is checked against. */
void gfx_quarter_select_scalar(const uint16_t* mine, const uint16_t* other, const uint8_t* keep, uint16_t* out,
                               int steps);

/* Both steps across, zipped: pixel x of mine becomes out[2x], its select
 * toward left[x], and out[2x + 1], toward right[x]. `keeps` holds `steps` keep
 * bytes for the left, then `steps` for the right. Every pointer but `keeps` is
 * at GFX_QUARTER_ALIGN. */
void gfx_quarter_spread(const uint16_t* mine, const uint16_t* left, const uint16_t* right, const uint8_t* keeps,
                        uint16_t* out, int steps);

/* The plain C body of gfx_quarter_spread(), as above. */
void gfx_quarter_spread_scalar(const uint16_t* mine, const uint16_t* left, const uint16_t* right, const uint8_t* keeps,
                               uint16_t* out, int steps);
