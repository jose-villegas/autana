/*
 * r3d_span_internal: the test that drops a walked triangle before its colour
 * planes, split out so a suite can check the decision itself. Only
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

/* True when every pixel of `box` already holds depth at or nearer than
 * `bound`, so a triangle bounded by it would write none of them. */
bool r3d_span_hidden(const r3d_span_target_t* target, int32_t bound, r3d_span_box_t box);
