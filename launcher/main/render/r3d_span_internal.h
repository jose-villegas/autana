/*
 * r3d_span_internal: what r3d_span.c shares with the rest of render/ and its
 * tests: the snap and pack helpers, the measurement switch, and the tests
 * that drop a walked triangle before its colour planes and let its spans
 * skip their clamps. Not a public module; r3d_span.h is.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "render/r3d_span.h"

typedef struct {
    int x0, x1, y0, y1; /* pixel centres, half-open, already inside the window */
} r3d_span_box_t;

/* An upper bound on the depth a triangle writes inside `box`, as the
 * 16-bit depth, from its depth plane in 16.8: `top` at the centre of pixel
 * (box.x0, box.y0), stepping `dx` a column and `dy` a row. */
int32_t r3d_span_plane_bound(int32_t top, int32_t dx, int32_t dy, r3d_span_box_t box);

/* True when a plane in the same form stays inside [0, max] at every pixel
 * centre of `box`, so a span there needs no clamp. */
bool r3d_span_plane_in_range(int32_t top, int32_t dx, int32_t dy, int32_t max, r3d_span_box_t box);

/* True when every pixel of `box` already holds depth at or nearer than
 * `bound`, so a triangle bounded by it would write none of them. */
bool r3d_span_hidden(const r3d_span_target_t* target, int32_t bound, r3d_span_box_t box);

/* Added before truncating, so the sum is positive and truncating floors. */
#define R3D_SNAP_BIAS ((float)R3D_SPAN_RANGE + 0.5F)

/* A subpixel position plus R3D_SNAP_BIAS, as a float, to the nearest
 * subpixel. `biased` must lie in (0, 2 R3D_SPAN_RANGE). */
static inline int32_t
r3d_span_unbias(float biased) {
    return (int32_t)biased - R3D_SPAN_RANGE;
}

/* A position in pixels, under 1024 from the origin, to the nearest subpixel. */
static inline int32_t
r3d_span_snap(float pixels) {
    return r3d_span_unbias((pixels * (float)R3D_SUBPIXEL) + R3D_SNAP_BIAS);
}

/* The first pixel whose centre is at or past subpixel position v. */
static inline int
r3d_span_first_centre(int32_t v) {
    return (v + (R3D_SUBPIXEL / 2) - 1) >> R3D_SUBPIXEL_SHIFT;
}

/* Channels as 8.8 fixed point (0..0xFF00), packed to the target's pixel
 * format. */
static inline uint16_t
r3d_span_pack(int32_t r, int32_t g, int32_t b) {
    const uint32_t native = ((uint32_t)r & 0xF800U) | (((uint32_t)g >> 5) & 0x07E0U) | ((uint32_t)b >> 11);
    return (uint16_t)((native >> 8) | (native << 8));
}

static inline int32_t
r3d_span_min3(int32_t a, int32_t b, int32_t c) {
    return a < b ? (a < c ? a : c) : (b < c ? b : c);
}

static inline int32_t
r3d_span_max3(int32_t a, int32_t b, int32_t c) {
    return a > b ? (a > c ? a : c) : (b > c ? b : c);
}

/* Temporary measurement switch: 0 draws normally; 1 stops after triangle
 * setup, 2 after walking the rows, 3 after each span's setup. A triangle
 * tested centre by centre stops after its setup under any of them. */
extern int r3d_span_stop_after;
