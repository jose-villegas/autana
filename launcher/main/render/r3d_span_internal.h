/*
 * r3d_span_internal: the tests that drop a walked triangle before its colour
 * planes and let its spans skip their clamps, split out so a suite can check
 * the decisions themselves. Only
 * r3d_span.c and its tests include this; it is not a public module.
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

/* Pixels x_first..x_last of one row in a face colour: depth `z` in 16.8 at
 * x_first, stepping `dz` a pixel, written with the colour where nearer. */
void r3d_span_fill_solid_row(uint16_t* depth, uint16_t* out, int x_first, int x_last, int32_t z, int32_t dz,
                             uint16_t color);
