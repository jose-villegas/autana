/*
 * r3d_project_x: r3d_project.h's line projection in Q16.16 integers, for a
 * caller that projects thousands of independent points a frame and wants the
 * integer unit's speed: no float conversion, no soft divide, a 32-bit
 * hardware divide per point. The view is built once a frame from a float
 * r3d_line_view_t, so the camera maths stays float.
 *
 * Camera-space points are int32 in 1/512 of a meter, the resolution a pixel
 * needs and one a 32-bit product with a Q9 focal length cannot overflow at.
 * The view's matrix entries are Q9 too, so mat4x_apply() of a Q16.16 point
 * (util/math/mathx.h) lands in that space with no further shift. Header-only,
 * static inline and ESP-IDF-free.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "render/r3d_project.h"
#include "util/math/mat4x.h"
#include "util/math/vec_convert.h"

/* One meter in camera space, and the focal length's one. */
#define R3D_X_UNIT_ONE 512

typedef struct {
    mat4x_t matrix; /* model * view, entries Q9: a Q16.16 point comes out in 1/512 m */
    int32_t focal;  /* projection-plane distance, 512 to 1.0; 0 is orthographic */
    int32_t near_z; /* camera-space clip plane, 1/512 m, > 0 */
    int center_x;   /* screen pixel the optical axis lands on */
    int center_y;
    int scale; /* pixels per projection-plane unit, both axes */
} r3d_line_view_x_t;

static inline r3d_line_view_x_t
r3d_line_view_to_x(const r3d_line_view_t* view) {
    r3d_line_view_x_t out;
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            out.matrix.m[r][c] = mathf_round_i32(view->matrix.m[r][c] * (float)R3D_X_UNIT_ONE);
        }
    }
    out.focal = mathf_round_i32(view->focal * (float)R3D_X_UNIT_ONE);
    out.near_z = mathf_round_i32(view->near_z * (float)R3D_X_UNIT_ONE);
    out.center_x = view->center_x;
    out.center_y = view->center_y;
    out.scale = (int)view->scale;
    return out;
}

/* Does not check z: a point at or behind the camera gives a defined pixel,
 * not a meaningful one. */
static inline void
r3d_camera_to_screen_x(vec3x_t p, const r3d_line_view_x_t* view, int* screen_x, int* screen_y) {
    int32_t x = p.x;
    int32_t y = p.y;
    if (view->focal != 0) {
        const int32_t z = p.z + (p.z == 0);
        x = (x * view->focal) / z;
        y = (y * view->focal) / z;
    }
    /* Only the multiply is 64-bit: a near-camera point's divided x and y can
     * overflow a 32-bit product with the scale. */
    *screen_x = (int)(view->center_x + (((int64_t)x * view->scale) / R3D_X_UNIT_ONE));
    *screen_y = (int)(view->center_y - (((int64_t)y * view->scale) / R3D_X_UNIT_ONE));
}

static inline bool
r3d_project_point_cs_x(vec3x_t p, const r3d_line_view_x_t* view, int* screen_x, int* screen_y) {
    if (p.z <= view->near_z) {
        return false;
    }
    r3d_camera_to_screen_x(p, view, screen_x, screen_y);
    return true;
}

/* Clips to the near plane; false if the segment is at or behind it. */
static inline bool
r3d_project_segment_cs_x(vec3x_t p0, vec3x_t p1, const r3d_line_view_x_t* view, int* ax, int* ay, int* bx, int* by) {
    const bool front0 = p0.z > view->near_z;
    const bool front1 = p1.z > view->near_z;

    if (!front0 && !front1) {
        return false;
    }

    if (front0 != front1) {
        vec3x_t* behind = front0 ? &p1 : &p0;
        const vec3x_t* front = front0 ? &p0 : &p1;
        const int64_t frac_q16 = ((int64_t)(view->near_z - behind->z) * 65536) / (front->z - behind->z);

        behind->x += (int32_t)(((int64_t)(front->x - behind->x) * frac_q16) >> 16);
        behind->y += (int32_t)(((int64_t)(front->y - behind->y) * frac_q16) >> 16);
        behind->z = view->near_z;
    }

    r3d_camera_to_screen_x(p0, view, ax, ay);
    r3d_camera_to_screen_x(p1, view, bx, by);
    return true;
}
