/*
 * render_view: one frame's world-to-picture basis and perspective fit.
 * The pose looks down its +z with +y up; picture y points down,
 * and the pose's scale does not affect the view.
 */
#pragma once

#include "display/viewport.h"
#include "math/linear/projectf.h"
#include "math/linear/transformf.h"

typedef enum { CAMERA_PRESENT_NEAREST, CAMERA_PRESENT_SCANLINE, CAMERA_PRESENT_DOUBLE } camera_present_t;

typedef struct {
    vec3f_t position, screen_x, screen_y, forward;
    projectf_t lens; /* Square pixels require fx == fy. */
    viewport_t viewport;
    camera_present_t present;
} render_view_t;

render_view_t render_view_make(const transformf_t* pose, float half_fov_short_tan, float near_z, viewport_t viewport);

/* Reprojects a retained pose through this picture's shape and turn. */
void render_view_refit(render_view_t* view, viewport_t viewport);
