/*
 * render_view: one frame's world-to-picture basis and perspective fit.
 * The pose looks down its +z with +y up; picture y points down,
 * and the pose's scale does not affect the view.
 */
#pragma once

#include <stdint.h>

#include "display/viewport.h"
#include "math/linear/projectf.h"
#include "math/linear/transformf.h"

typedef enum { CAMERA_PRESENT_NEAREST, CAMERA_PRESENT_SCANLINE, CAMERA_PRESENT_DOUBLE } camera_present_t;

typedef enum { SMALL_FILL_FLAT, SMALL_FILL_GRAIN, SMALL_FILL_DITHER, SMALL_FILL_STYLE_COUNT } small_fill_style_t;

#define SMALL_FILL_CEILING_DEFAULT 12
#define SMALL_FILL_GATE_DEFAULT    16
#define SMALL_FILL_STYLE_DEFAULT   SMALL_FILL_FLAT
#define SMALL_FILL_DEFAULT                                                                                             \
    ((small_fill_t){SMALL_FILL_CEILING_DEFAULT, SMALL_FILL_GATE_DEFAULT, SMALL_FILL_STYLE_DEFAULT})

typedef struct {
    uint8_t ceiling;          /* largest bounding-box side in pixel centres; 0 disables small fill */
    uint8_t gate;             /* RGB565 per-channel spread at the ceiling, in packed channel units; 0 disables gating */
    small_fill_style_t style; /* colour kernel above the always-flat side limit */
} small_fill_t;

typedef struct {
    vec3f_t position, screen_x, screen_y, forward;
    float pixels_per_unit, center_x, center_y, near_z;
    viewport_t viewport;
    camera_present_t present;
    small_fill_t small_fill;
} render_view_t;

/* The view as a pinhole lens: one focal length on both axes. */
static inline projectf_t
render_view_lens(const render_view_t* view) {
    return (projectf_t){view->pixels_per_unit, view->pixels_per_unit, view->center_x, view->center_y, view->near_z};
}

render_view_t render_view_make(const transformf_t* pose, float half_fov_short_tan, float near_z, viewport_t viewport);

/* Reprojects a retained pose through this picture's shape and turn. */
void render_view_refit(render_view_t* view, viewport_t viewport);
