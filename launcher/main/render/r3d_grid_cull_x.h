/*
 * r3d_grid_cull_x: which points of a lifted grid the fixed-point projection
 * certainly puts off a clip, so a walk of the grid's edges can skip them.
 * Not planef_frustum(): these bounds carry r3d_camera_to_screen_x()'s own
 * rounding, so a skipped point is one that projection would reject.
 */
#pragma once

#include <stdint.h>

#include "math/geometry/recti.h"
#include "render/r3d_project_x.h"

/* The most columns and rows a grid has, past one: a span is an int8_t. */
#define R3D_X_GRID_K_MAX (INT8_MAX - 1)

/* Points the narrow transform sums, before its shift, as
 * base + k * step + r * row + h * rise for columns 0 <= k <= k_max, rows
 * 0 <= r <= r_max and every lift |h| <= h_max. The sums must fit it. */
typedef struct {
    vec3x_t base;
    vec3x_t step;
    vec3x_t row;
    vec3x_t rise;
    int32_t h_max;
    int k_max;
    int r_max;
} r3d_x_grid_t;

/* For each row r, keep[r] = {lo, hi}, the columns a walk of the grid's
 * four-neighbour edges must visit (empty when lo > hi). Every point
 * outside them shares with all four of its neighbours one certainty, at
 * every lift: behind the near plane, or in front of it and past one side
 * of the clip. So each edge from it projects hidden: both ends behind, or
 * both sharing a gfx_clip_outcode() bit. */
void r3d_x_grid_cull_spans(const r3d_line_view_x_t* view, const recti_t* clip, const r3d_x_grid_t* grid,
                           int8_t keep[][2]);
