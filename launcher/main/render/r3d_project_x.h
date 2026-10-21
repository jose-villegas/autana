/* r3d_project_x: fixed-point line projection with a 32-bit per-point divide.
 * Math's projectx_t lens; 1/512 m camera space from Q16.16 model points. ESP-IDF-free. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "math/linear/mat4x.h"
#include "math/linear/projectx.h"
#include "math/linear/vec_convert.h"
#include "math/scalar/mathx.h"
#include "render/render_view.h"

/* Camera space is +y up; the camera pose's scale has no effect. */
static inline mat4f_t
r3d_line_matrix(const render_view_t* view, const transformf_t* model) {
    return mat4f_mul(
        mat4f_view_from_axes(view->screen_x, vec3f_scale(view->screen_y, -1.0F), view->forward, view->position),
        transformf_compute_matrix(model));
}

/* One meter in camera space. */
#define R3D_X_UNIT_SHIFT        9
#define R3D_X_UNIT_ONE          (1 << R3D_X_UNIT_SHIFT)

/* A near plane for render_view_make() on the line path, in world units,
 * which this header's camera space reads as meters. */
#define R3D_LINE_NEAR_Z         0.1F

/* The shift of the narrow transform's sum: entries carry 12 more bits. */
#define R3D_X_INPUT_SHIFT       12

/* Entry bound times input bound, three terms plus translation, fits the
 * 32-bit sum. Entry and translation bounds are exclusive; input bounds
 * are inclusive at -LIMIT and exclusive at +LIMIT. */
#define R3D_X_ENTRY_XZ_LIMIT    (1 << 12)
#define R3D_X_ENTRY_Y_LIMIT     (1 << 14)
#define R3D_X_TRANSLATION_LIMIT (1 << 27)
#define R3D_X_INPUT_XZ_LIMIT    (1 << 17)
#define R3D_X_INPUT_Y_LIMIT     (1 << 15)

_Static_assert(2LL * (R3D_X_ENTRY_XZ_LIMIT - 1) * R3D_X_INPUT_XZ_LIMIT
                       + (int64_t)(R3D_X_ENTRY_Y_LIMIT - 1) * R3D_X_INPUT_Y_LIMIT + (R3D_X_TRANSLATION_LIMIT - 1)
                   <= INT32_MAX,
               "narrow transform sum must fit int32_t");

typedef struct {
    int32_t units[3][4]; /* the matrix scaled for whole-unit inputs, see below */
    bool units_ok;       /* every entry is small enough for a 32-bit sum */
    mat4x_t matrix;      /* view * model, entries Q9: a Q16.16 point comes out in 1/512 m */
    projectx_t lens;     /* Q16.16 pixels; near_z in Q16.16 m, read via r3d_line_near_z() */
} r3d_line_projector_t;

/* units[r][c] is matrix entry [r][c] times meters_per_unit[c], and
 * units[r][3] the translation, all in 1/512 m with R3D_X_INPUT_SHIFT more
 * bits, so r3d_to_camera_space_units() is three 32-bit products and a shift.
 * units_ok guarantees that sum fits for raw inputs within
 * R3D_X_INPUT_XZ_LIMIT on axes 0 and 2 and R3D_X_INPUT_Y_LIMIT on axis 1;
 * the caller checks those inputs, else uses mat4x_apply(). */
static inline r3d_line_projector_t
r3d_line_projector_make(const render_view_t* view, const transformf_t* model, const float meters_per_unit[3]) {
    const mat4f_t matrix = r3d_line_matrix(view, model);
    r3d_line_projector_t out;
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            out.matrix.m[r][c] = mathf_round_i32(matrix.m[r][c] * (float)R3D_X_UNIT_ONE);
        }
    }
    out.lens = projectx_from_projectf(&view->lens);
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
r3d_to_camera_space_units(const r3d_line_projector_t* view, int32_t a, int32_t b, int32_t c) {
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
r3d_camera_to_screen_x(vec3x_t p, const r3d_line_projector_t* view, int* screen_x, int* screen_y) {
    const int32_t z = p.z + (p.z == 0);
    const int32_t x = (p.x * R3D_X_UNIT_ONE) / z;
    const int32_t y = (p.y * R3D_X_UNIT_ONE) / z;
    /* Near-camera offsets times Q16.16 focal lengths need a 64-bit product. */
    *screen_x =
        (view->lens.cx >> MATHX_SHIFT) + (int)(((int64_t)x * view->lens.fx) / ((int64_t)R3D_X_UNIT_ONE << MATHX_SHIFT));
    *screen_y =
        (view->lens.cy >> MATHX_SHIFT) - (int)(((int64_t)y * view->lens.fy) / ((int64_t)R3D_X_UNIT_ONE << MATHX_SHIFT));
}

static inline int32_t
r3d_line_near_z(const r3d_line_projector_t* view) {
    return fx_round_shift32(view->lens.near_z, MATHX_SHIFT - R3D_X_UNIT_SHIFT);
}

static inline bool
r3d_project_point_cs_x(vec3x_t p, const r3d_line_projector_t* view, int* screen_x, int* screen_y) {
    if (p.z <= r3d_line_near_z(view)) {
        return false;
    }
    r3d_camera_to_screen_x(p, view, screen_x, screen_y);
    return true;
}

/* Clips to the near plane; false if the segment is at or behind it. */
static inline bool
r3d_project_segment_cs_x(vec3x_t p0, vec3x_t p1, const r3d_line_projector_t* view, int* ax, int* ay, int* bx, int* by) {
    if (!projectx_clip_near(r3d_line_near_z(view), &p0, &p1)) {
        return false;
    }

    r3d_camera_to_screen_x(p0, view, ax, ay);
    r3d_camera_to_screen_x(p1, view, bx, by);
    return true;
}
