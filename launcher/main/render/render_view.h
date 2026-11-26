/* render_view: one frame's world-to-picture basis and perspective fit.
 * Pose: +z forward, +y up, scale ignored. Picture: +y down. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "display/viewport.h"
#include "math/linear/transformf.h"
#include "math/projection/projectf.h"

typedef enum { CAMERA_PRESENT_NEAREST, CAMERA_PRESENT_SCANLINE, CAMERA_PRESENT_DOUBLE } camera_present_t;

typedef struct {
    vec3f_t position, screen_x, screen_y, forward;
    projectf_t lens; /* Square pixels: fx == fy. */
    viewport_t viewport;
    camera_present_t present;
    /* Skip each cluster wholly behind depth already drawn, before any of its
     * triangles is set up. It pays where much geometry hides behind nearer
     * geometry; elsewhere the test costs more than it skips, so it is off
     * unless a scene turns it on. It changes no pixel the view keeps. */
    bool skip_hidden;
    /* The lens shift: the optical centre's offset from the picture's middle,
     * along the camera's right and up, in picture short sides. Zero looks
     * straight ahead; a window onto a fixed rectangle moves the eye and shifts
     * the lens back so the rectangle stays framed (an off-axis frustum). */
    vec2f_t shift;
} render_view_t;

render_view_t render_view_make(const transformf_t* pose, float half_fov_short_tan, float near_z, viewport_t viewport);

/* The tangent of half the field of view across the picture's shorter side,
 * as render_view_make() took it. */
float render_view_half_fov_short_tan(const render_view_t* view);

/* Shifts the lens (render_view_t.shift) and recentres the picture on it. */
void render_view_set_shift(render_view_t* view, vec2f_t shift);

/* Reprojects a retained pose through this picture's shape and turn. */
void render_view_refit(render_view_t* view, viewport_t viewport);

/* The pose `view` was made from, render_view_make()'s inverse to rounding:
 * unit scale, whatever scale the pose had. */
transformf_t render_view_pose(const render_view_t* view);
