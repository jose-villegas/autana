/*
 * render_view: one frame's world-to-picture basis and perspective fit.
 * The pose looks down its +z with +y up; picture y points down,
 * and the pose's scale does not affect the view.
 */
#pragma once

#include "render/viewport.h"
#include "util/math/transformf.h"

typedef struct {
    vec3f_t position, screen_x, screen_y, forward;
    float pixels_per_unit, center_x, center_y, near_z;
    viewport_t viewport;
} render_view_t;

render_view_t render_view_make(const transformf_t* pose, float half_fov_short_tan, float near_z, viewport_t viewport);

/* Reprojects a retained pose through this picture's shape and turn. */
void render_view_refit(render_view_t* view, viewport_t viewport);
