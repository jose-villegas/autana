/*
 * r3d_span_internal: what r3d_span.c shares with the rest of render/ and its
 * tests: the snap and pack helpers, the measurement switch, and the tests
 * that drop a walked triangle before its colour planes and let its spans
 * skip their clamps. Not a public module; r3d_span.h is.
 */
#pragma once

#include "math/geometry/recti.h"

#include <stdbool.h>
#include <stdint.h>

#include "math/scalar/mathf.h"
#include "render/r3d_span.h"

#define R3D_SPAN_COLOR_SCALE 256.0F

/* An upper bound on the depth a triangle writes inside `box`, as the
 * 16-bit depth, from its depth plane in 16.8: `top` at the centre of pixel
 * (box.x, box.y), stepping `dx` a column and `dy` a row. */
int32_t r3d_span_plane_bound(int32_t top, int32_t dx, int32_t dy, recti_t box);

/* True when a plane in the same form stays inside [0, max] at every pixel
 * centre of `box`, so a span there needs no clamp. */
bool r3d_span_plane_in_range(int32_t top, int32_t dx, int32_t dy, int32_t max, recti_t box);

/* True when every pixel of `box` already holds depth at or nearer than
 * `bound`, so a triangle bounded by it would write none of them. Kept out
 * of line, as its layout is pinned. */
__attribute__((noinline)) bool r3d_span_hidden(const r3d_span_target_t* target, int32_t bound, recti_t box);

/* The part of `centres` inside `target`'s window: its full width, its own
 * rows. Runs once per triangle, so a miss is left as a size at or below zero
 * (recti_is_empty reads it as empty) rather than clamped to zero. */
static inline recti_t
r3d_span_window_box(const r3d_span_target_t* target, recti_t centres) {
    const int left = mathi_max(centres.x, 0);
    const int top = mathi_max(centres.y, target->rows.first_row);
    const recti_t box = {left, top, mathi_min(recti_right(centres), target->rows.attachment[0].width) - left,
                         mathi_min(recti_bottom(centres), gfx_target_end_row(&target->rows)) - top};
    return box;
}

/* How far a fill's 16-bit depth can rise above its nearest vertex's,
 * z * R3D_DEPTH_NEAREST truncated: one for the float set-up's rounding at its
 * first centre, and one 16.8 unit for each pixel stepped from it, as a step
 * is truncated, across the widest box a span takes, both axes. */
#define R3D_SPAN_DEPTH_DRIFT (1 + ((2 * 2 * R3D_SPAN_RANGE / R3D_SUBPIXEL) >> R3D_DEPTH_SHIFT))

/* Added before truncating, so the sum is positive and truncating floors. */
#define R3D_SNAP_BIAS        ((float)R3D_SPAN_RANGE + 0.5F)

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

static inline uint16_t
r3d_span_pack_rgb(float r, float g, float b) {
    return r3d_span_pack((int32_t)(mathf_clamp(r, 0.0F, UINT8_MAX) * R3D_SPAN_COLOR_SCALE),
                         (int32_t)(mathf_clamp(g, 0.0F, UINT8_MAX) * R3D_SPAN_COLOR_SCALE),
                         (int32_t)(mathf_clamp(b, 0.0F, UINT8_MAX) * R3D_SPAN_COLOR_SCALE));
}

static inline int32_t
r3d_span_min3(int32_t a, int32_t b, int32_t c) {
    return a < b ? (a < c ? a : c) : (b < c ? b : c);
}

static inline int32_t
r3d_span_max3(int32_t a, int32_t b, int32_t c) {
    return a > b ? (a > c ? a : c) : (b > c ? b : c);
}

/* Small enough that a gradient across it is invisible: one colour, one
 * depth. Its coverage is still decided by the same edges. */
#define R3D_SPAN_FLAT_MAX_ROWS  2
#define R3D_SPAN_FLAT_MAX_WIDTH (3 * R3D_SUBPIXEL)

/* A triangle's pixel centres in its bounding box, over the whole screen and
 * not a window's rows: what r3d_span chooses its path by, so every window
 * takes the same path for it. */
typedef struct {
    recti_t centres; /* pixel centres, not clipped */
    bool flat;       /* one colour and one depth stand in for its planes */
} r3d_span_extent_t;

static inline r3d_span_extent_t
r3d_span_extent_limits(const r3d_span_vertex_t* a, const r3d_span_vertex_t* b, const r3d_span_vertex_t* c, int max_rows,
                       int max_width) {
    const int32_t lo_x = r3d_span_min3(a->x, b->x, c->x);
    const int32_t hi_x = r3d_span_max3(a->x, b->x, c->x);
    /* first_centre keeps order, so the edges never cross and need no clamp. */
    const int left = r3d_span_first_centre(lo_x);
    const int top = r3d_span_first_centre(r3d_span_min3(a->y, b->y, c->y));
    const recti_t centres = {left, top, r3d_span_first_centre(hi_x) - left,
                             r3d_span_first_centre(r3d_span_max3(a->y, b->y, c->y)) - top};
    return (r3d_span_extent_t){centres, centres.h <= max_rows && hi_x - lo_x <= max_width};
}

static inline r3d_span_extent_t
r3d_span_extent(const r3d_span_vertex_t* a, const r3d_span_vertex_t* b, const r3d_span_vertex_t* c) {
    return r3d_span_extent_limits(a, b, c, R3D_SPAN_FLAT_MAX_ROWS, R3D_SPAN_FLAT_MAX_WIDTH);
}

/* Twice the signed area of a-b-c; each product is under 2^30. */
static inline int32_t
r3d_span_area2(const r3d_span_vertex_t* a, const r3d_span_vertex_t* b, const r3d_span_vertex_t* c) {
    return ((b->x - a->x) * (c->y - a->y)) - ((b->y - a->y) * (c->x - a->x));
}

/* Temporary measurement switch: where the fill stops. Stopping after the
 * carry also stops attachment writers. A triangle tested centre by centre
 * stops after its setup under any of them. */
enum {
    R3D_SPAN_STOP_NONE,        /* draws normally */
    R3D_SPAN_STOP_AFTER_SETUP, /* after triangle setup */
    R3D_SPAN_STOP_AFTER_EDGES, /* after walking only the edges */
    R3D_SPAN_STOP_AFTER_CARRY, /* after the attribute carry */
};

extern int r3d_span_stop_after;
