/*
 * r3d_span_internal: what r3d_span.c shares with the rest of render/ and its
 * tests: the snap and pack helpers, the edge and row helpers the screen fill
 * (r3d_span_screen.c) also uses, the measurement switch, and the tests that
 * drop a walked triangle before its colour planes and let its spans skip
 * their clamps. Not a public module; r3d_span.h is.
 */
#pragma once

#include "math/geometry/recti.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "math/scalar/mathf.h"
#include "render/r3d_span.h"

/* Span colour channels are fixed point with this many fraction bits (8.8). */
#define R3D_SPAN_COLOR_FRAC_BITS 8
#define R3D_SPAN_COLOR_SCALE     ((float)(1 << R3D_SPAN_COLOR_FRAC_BITS))

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

/* The nearest 16-bit depth a fill from vertices no nearer than `iz` writes.
 * The drift covers a plane set up from a triangle of real area; a sliver's
 * float set-up can stray past it, so the fill clamps every depth it writes
 * to this. Saturates, as the fill does. */
static inline uint16_t
r3d_span_depth_limit(float iz) {
    const int32_t depth = (int32_t)(iz * (float)R3D_DEPTH_NEAREST) + R3D_SPAN_DEPTH_DRIFT;
    return (uint16_t)(depth < R3D_DEPTH_NEAREST ? depth : R3D_DEPTH_NEAREST);
}

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

/* The pixel centres inside a subpixel box, over the whole screen; empty when
 * it holds none. first_centre keeps order, so the edges never cross. */
static inline recti_t
r3d_span_centres(int32_t lo_x, int32_t lo_y, int32_t hi_x, int32_t hi_y) {
    const int left = r3d_span_first_centre(lo_x);
    const int top = r3d_span_first_centre(lo_y);
    return (recti_t){left, top, r3d_span_first_centre(hi_x) - left, r3d_span_first_centre(hi_y) - top};
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
    const recti_t centres =
        r3d_span_centres(lo_x, r3d_span_min3(a->y, b->y, c->y), hi_x, r3d_span_max3(a->y, b->y, c->y));
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

#define R3D_SPAN_HALF_PIXEL (R3D_SUBPIXEL / 2)

typedef struct {
    uint16_t* depth;
    uint16_t* color;
    int32_t centre;
} r3d_span_row_t;

static inline __attribute__((always_inline)) r3d_span_row_t
r3d_span_row(const r3d_span_target_t* target, int y) {
    const gfx_target_t* rows = &target->rows;
    const size_t offset = (size_t)(y - rows->first_row) * rows->attachment[GFX_ATTACHMENT_COLOR].pitch;
    return (r3d_span_row_t){(uint16_t*)((char*)rows->attachment[GFX_ATTACHMENT_DEPTH].pixels + offset),
                            (uint16_t*)((char*)rows->attachment[GFX_ATTACHMENT_COLOR].pixels + offset),
                            (y << R3D_SUBPIXEL_SHIFT) + R3D_SPAN_HALF_PIXEL};
}

/* Edge a-b of a triangle winding positive, where (b - a) x (p - a) is above
 * zero inside; a centre exactly on it belongs to it only on a top or left
 * edge, which is the walk's rule in the same integers. */
typedef struct {
    int32_t ax, ay, dx, dy, bias;
} r3d_span_edge_t;

static inline r3d_span_edge_t
r3d_span_edge(const r3d_span_vertex_t* a, const r3d_span_vertex_t* b) {
    const int32_t dx = b->x - a->x;
    const int32_t dy = b->y - a->y;
    const bool top_left = dy < 0 || (dy == 0 && dx > 0);
    return (r3d_span_edge_t){a->x, a->y, dx, dy, top_left ? 0 : 1};
}

/* Negative when the centre is outside. */
static inline int32_t
r3d_span_side(const r3d_span_edge_t* e, int32_t x, int32_t y) {
    return (e->dx * (y - e->ay)) - (e->dy * (x - e->ax)) - e->bias;
}

/* An edge's first column at or right of it, row by row, exactly: ceil(n /
 * d) for an n growing a fixed step per row, held as quotient q and
 * remainder r in [0, d). Two triangles sharing the edge get the same column
 * on every row in any window, so one fills up to it and the other from it. */
typedef struct {
    int32_t q, r, qs, rs, d;
} r3d_span_walk_edge_t;

static inline void
r3d_span_floor_div(int32_t n, int32_t d, int32_t* q, int32_t* r) {
    *q = n / d;
    *r = n - (*q * d);
    if (*r < 0) {
        *q -= 1;
        *r += d;
    }
}

/* Row y's centre line crosses the edge at x = top.x + (16 y + 8 - top.y)
 * dx / dy, and the first centre at or right of it is ceil((x - 8) / 16).
 * With every coordinate inside R3D_SPAN_RANGE the numerator stays under
 * 2^31 for any row of the edge. */
static __attribute__((unused)) r3d_span_walk_edge_t
r3d_span_walk_edge_at(const r3d_span_vertex_t* top, const r3d_span_vertex_t* bottom, int y) {
    const int32_t dx = bottom->x - top->x;
    const int32_t dy = bottom->y - top->y;
    const int32_t row_offset = (y << R3D_SUBPIXEL_SHIFT) + R3D_SPAN_HALF_PIXEL - top->y;
    r3d_span_walk_edge_t e = {0, 0, 0, 0, dy << R3D_SUBPIXEL_SHIFT};
    r3d_span_floor_div(((top->x - R3D_SPAN_HALF_PIXEL) * dy) + (row_offset * dx) + e.d - 1, e.d, &e.q, &e.r);
    r3d_span_floor_div(dx, dy, &e.qs, &e.rs);
    e.rs <<= R3D_SUBPIXEL_SHIFT;
    return e;
}

static inline __attribute__((always_inline)) bool
r3d_span_walk_edge_step(r3d_span_walk_edge_t* e) {
    e->q += e->qs;
    e->r += e->rs;
    if (e->r >= e->d) {
        e->r -= e->d;
        e->q++;
        return true;
    }
    return false;
}

/* Returns true when the sort swapped an odd number of times, which turns
 * the triangle's winding over. */
static inline bool
r3d_span_sort_by_y(const r3d_span_vertex_t** v0, const r3d_span_vertex_t** v1, const r3d_span_vertex_t** v2) {
    const r3d_span_vertex_t* t;
    bool odd = false;
    if ((*v1)->y < (*v0)->y) {
        t = *v0, *v0 = *v1, *v1 = t, odd = !odd;
    }
    if ((*v2)->y < (*v1)->y) {
        t = *v1, *v1 = *v2, *v2 = t, odd = !odd;
    }
    if ((*v1)->y < (*v0)->y) {
        t = *v0, *v0 = *v1, *v1 = t, odd = !odd;
    }
    return odd;
}

/* A pixel centre just outside the triangle extrapolates past the vertex
 * range, and a wrapped channel would be a wrong-coloured pixel. Each span's
 * start is clamped; only a span whose end would still leave the range pays
 * for a step recomputed from both clamped ends. */
static inline void
r3d_span_clamp_to(int32_t max, int count, int32_t* value, int32_t* step) {
    const int32_t start = mathi_clamp(*value, 0, max);
    const int32_t end = start + (*step * count);
    *value = start;
    if (count > 0 && (end < 0 || end > max)) {
        *step = (mathi_clamp(end, 0, max) - start) / count;
    }
}

/* Every writer of the target over one span. */
static inline void
r3d_span_write(const r3d_span_target_t* target, int y, int x_first, int x_last, int32_t z, int32_t dz) {
    for (int i = 0; i < target->writer_count; i++) {
        target->writers[i].span(&target->writers[i], &target->rows, y, x_first, x_last, z, dz);
    }
}
