/*
 * r3d_project: camera-space near-plane clip and perspective projection.
 * Float reference the fixed-point line path is tested against, with no subpixel snap
 * or guard band. Header-only and ESP-IDF-free.
 */
#pragma once

#include <math.h>
#include <stdbool.h>

#include "math/linear/vec3f.h"
#include "math/scalar/mathf.h"
#include "render/r3d_project_common.h"
#include "render/render_view.h"

static inline void
r3d_project_screen(vec3f_t p, const render_view_t* view, int* screen_x, int* screen_y) {
    const float gain = view->pixels_per_unit * mathf_recip(p.z);
    *screen_x = (int)view->center_x + r3d_pixel_offset(p.x * gain);
    *screen_y = (int)view->center_y - r3d_pixel_offset(p.y * gain);
}

/* False when the point is at or behind the near plane. */
static inline bool
r3d_project_point_cs(vec3f_t p, const render_view_t* view, int* screen_x, int* screen_y) {
    if (p.z <= view->near_z) {
        return false;
    }
    r3d_project_screen(p, view, screen_x, screen_y);
    return true;
}

/* Clips to near plane; avoids screen wrap. Returns false if segment is at or
 * behind the plane. */
static inline bool
r3d_project_segment_cs(vec3f_t p0, vec3f_t p1, const render_view_t* view, int* ax, int* ay, int* bx, int* by) {
    const bool front0 = p0.z > view->near_z;
    const bool front1 = p1.z > view->near_z;

    if (!front0 && !front1) {
        return false;
    }

    if (front0 != front1) {
        /* Replace endpoint with crossing point using linear interpolation in
         * camera space. */
        vec3f_t* behind = front0 ? &p1 : &p0;
        const vec3f_t* front = front0 ? &p0 : &p1;
        const float frac = (view->near_z - behind->z) / (front->z - behind->z);

        behind->x += (front->x - behind->x) * frac;
        behind->y += (front->y - behind->y) * frac;
        behind->z = view->near_z;
    }

    r3d_project_screen(p0, view, ax, ay);
    r3d_project_screen(p1, view, bx, by);
    return true;
}
