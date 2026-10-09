/*
 * render_view: one frame's world-to-picture basis and perspective fit.
 * Poses use +x right, +y up, +z forward in a left-handed frame;
 * picture y points down. Pose scale does not affect the view.
 */
#pragma once

#include "render/viewport.h"
#include "util/math/transformf.h"

typedef struct {
    vec3f_t position, screen_x, screen_y, forward;
    float pixels_per_unit, center_x, center_y, near_z;
    int width, height;
} render_view_t;

static inline render_view_t
render_view_make(const transformf_t* pose, float half_fov_short_tan, float near_z, viewport_t viewport) {
    /* Baked scenes use a right-handed frame, so picture right is pose -x. */
    const vec3f_t right = quatf_rotate(pose->rotation, (vec3f_t){-1.0F, 0.0F, 0.0F});
    const vec3f_t down = quatf_rotate(pose->rotation, (vec3f_t){0.0F, -1.0F, 0.0F});
    const viewport_quarter_axes_t a = viewport_quarter_axes(viewport.quarter);
    const int shorter = viewport.width < viewport.height ? viewport.width : viewport.height;
    return (render_view_t){
        .position = pose->position,
        .screen_x = vec3f_add(vec3f_scale(right, (float)a.x_right), vec3f_scale(down, (float)a.x_down)),
        .screen_y = vec3f_add(vec3f_scale(right, (float)a.y_right), vec3f_scale(down, (float)a.y_down)),
        .forward = quatf_rotate(pose->rotation, (vec3f_t){0.0F, 0.0F, 1.0F}),
        .pixels_per_unit = (float)shorter / (2.0F * half_fov_short_tan),
        .center_x = (float)viewport.width * 0.5F,
        .center_y = (float)viewport.height * 0.5F,
        .near_z = near_z,
        .width = viewport.width,
        .height = viewport.height,
    };
}
