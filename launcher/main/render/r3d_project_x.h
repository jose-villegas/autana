/*
 * r3d_project_x: fixed-point line projection with a 32-bit per-point divide.
 * Owns the camera-space model matrix, built per frame from render_view_t
 * and a model. Matrix entries and camera points carry R3D_X_UNIT_ONE per meter. Q16.16 model points need no further
 * shift after mat4x_apply(). Header-only and ESP-IDF-free.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "math/linear/mat4x.h"
#include "math/linear/vec_convert.h"
#include "math/scalar/mathx.h"
#include "render/r3d_project_common.h"
#include "render/render_view.h"

/* Camera space is +y up; the camera pose's scale has no effect. */
static inline mat4f_t
r3d_line_matrix(const render_view_t* view, const transformf_t* model) {
    mat4f_t camera = mat4f_identity();
    const vec3f_t axes[] = {view->screen_x, vec3f_scale(view->screen_y, -1.0F), view->forward};
    for (int r = 0; r < 3; r++) {
        camera.m[r][0] = axes[r].x;
        camera.m[r][1] = axes[r].y;
        camera.m[r][2] = axes[r].z;
        camera.m[r][3] = -vec3f_dot(axes[r], view->position);
    }
    return mat4f_mul(camera, transformf_compute_matrix(model));
}

/* One meter in camera space. */
#define R3D_X_UNIT_ONE          512

/* The shift of the narrow transform's sum: entries carry 12 more bits. */
#define R3D_X_INPUT_SHIFT       12

#define R3D_X_ENTRY_XZ_LIMIT    (1 << 12)
#define R3D_X_ENTRY_Y_LIMIT     (1 << 14)
#define R3D_X_TRANSLATION_LIMIT (1 << 27)

typedef struct {
    int32_t units[3][4]; /* the matrix scaled for whole-unit inputs, see below */
    bool units_ok;       /* every entry is small enough for a 32-bit sum */
    mat4x_t matrix;      /* view * model, entries Q9: a Q16.16 point comes out in 1/512 m */
    int32_t near_z;      /* camera-space clip plane, 1/512 m, > 0 */
    int center_x;        /* screen pixel the optical axis lands on */
    int center_y;
    int pixels_per_unit; /* pixels per projection-plane unit, both axes */
} r3d_line_view_x_t;

/* Fills the narrow form: entry [r][c] is the matrix entry times the meters
 * one raw unit of input c is worth, in 1/512 m with 12 more bits, so
 * r3d_to_camera_space_units() is three 32-bit products and a shift. `units_ok`
 * says no entry is too big (scale up to about 8, camera within 64 m) for inputs
 * inside +-2^17 on axes 0 and 2 and +-2^15 on axis 1 to overflow; the caller
 * checks the inputs, else uses mat4x_apply(). */
static inline r3d_line_view_x_t
r3d_line_view_x_make(const render_view_t* view, const transformf_t* model, const float meters_per_unit[3]) {
    const mat4f_t matrix = r3d_line_matrix(view, model);
    r3d_line_view_x_t out;
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            out.matrix.m[r][c] = mathf_round_i32(matrix.m[r][c] * (float)R3D_X_UNIT_ONE);
        }
    }
    out.near_z = mathf_round_i32(view->near_z * (float)R3D_X_UNIT_ONE);
    out.center_x = (int)view->center_x;
    out.center_y = (int)view->center_y;
    out.pixels_per_unit = mathf_round_i32(view->pixels_per_unit);
    const float scale = (float)R3D_X_UNIT_ONE * (float)(1 << R3D_X_INPUT_SHIFT);
    out.units_ok = true;
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) {
            out.units[r][c] = mathf_round_i32(matrix.m[r][c] * meters_per_unit[c] * scale);
            const int limit = c == 1 ? R3D_X_ENTRY_Y_LIMIT : R3D_X_ENTRY_XZ_LIMIT;
            out.units_ok = out.units_ok && out.units[r][c] < limit && out.units[r][c] > -limit;
        }
        out.units[r][3] = mathf_round_i32(matrix.m[r][3] * scale);
        out.units_ok =
            out.units_ok && out.units[r][3] < R3D_X_TRANSLATION_LIMIT && out.units[r][3] > -R3D_X_TRANSLATION_LIMIT;
    }
    return out;
}

/* The narrow transform: raw inputs a, b, c to camera space in 1/512 m. */
static inline vec3x_t
r3d_to_camera_space_units(const r3d_line_view_x_t* view, int32_t a, int32_t b, int32_t c) {
    return (vec3x_t){
        mathx_dot3_narrow(view->units[0][0], a, view->units[0][1], b, view->units[0][2], c, view->units[0][3],
                          R3D_X_INPUT_SHIFT),
        mathx_dot3_narrow(view->units[1][0], a, view->units[1][1], b, view->units[1][2], c, view->units[1][3],
                          R3D_X_INPUT_SHIFT),
        mathx_dot3_narrow(view->units[2][0], a, view->units[2][1], b, view->units[2][2], c, view->units[2][3],
                          R3D_X_INPUT_SHIFT),
    };
}

/* Does not check z: a point at or behind the camera gives a defined pixel,
 * not a meaningful one. */
static inline void
r3d_camera_to_screen_x(vec3x_t p, const r3d_line_view_x_t* view, int* screen_x, int* screen_y) {
    const int32_t z = p.z + (p.z == 0);
    const int32_t x = (p.x * R3D_X_UNIT_ONE) / z;
    const int32_t y = (p.y * R3D_X_UNIT_ONE) / z;
    /* Only the multiply is 64-bit: a near-camera point's divided x and y can
     * overflow a 32-bit product with pixels_per_unit. */
    *screen_x = (int)(view->center_x + (((int64_t)x * view->pixels_per_unit) / R3D_X_UNIT_ONE));
    *screen_y = (int)(view->center_y - (((int64_t)y * view->pixels_per_unit) / R3D_X_UNIT_ONE));
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
        const int64_t frac_q16 = ((int64_t)(view->near_z - behind->z) * MATHX_ONE) / (front->z - behind->z);

        behind->x += (int32_t)(((int64_t)(front->x - behind->x) * frac_q16) >> MATHX_SHIFT);
        behind->y += (int32_t)(((int64_t)(front->y - behind->y) * frac_q16) >> MATHX_SHIFT);
        behind->z = view->near_z;
    }

    r3d_camera_to_screen_x(p0, view, ax, ay);
    r3d_camera_to_screen_x(p1, view, bx, by);
    return true;
}
