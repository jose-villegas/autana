/*
 * r3d_ray - a float ray camera: origin plus an orthonormal forward/right/up
 * basis, and the direction of the ray through a physical pixel - the
 * quarter-turn physical-to-upright mapping and a lens fitted to the
 * viewport's SHORTER axis, on the same r3d_viewport_t a rasteriser uses.
 *
 * SINGLE PRECISION ONLY. The FPU this runs on has no double, so one stray
 * promotion costs an order of magnitude; a .c including this carries
 * `#pragma GCC diagnostic error "-Wdouble-promotion"` itself, since a pragma
 * in a header would bind every includer. The pose is float rather than
 * S3L_F units because a caller's numbers need not be representable there.
 */
#pragma once

#include <stdbool.h>

#include "render/r3d_camera.h"
#include "render/r3d_vec3f.h"

typedef struct {
    r3d_vec3f_t origin, forward, right, up;
    float half_fov_short_tan;
    r3d_viewport_t viewport;
} r3d_ray_camera_t;

static inline void
r3d_ray_camera_init(r3d_ray_camera_t* cam, r3d_vec3f_t origin, r3d_vec3f_t forward, r3d_vec3f_t right, r3d_vec3f_t up,
                    float half_fov_short_tan, r3d_viewport_t viewport) {
    cam->origin = origin;
    cam->forward = forward;
    cam->right = right;
    cam->up = up;
    cam->half_fov_short_tan = half_fov_short_tan;
    cam->viewport = viewport;
}

/* How the panel's own axes lie in the upright picture once it is read at
 * `quarter`: a step along physical x moves the upright pixel by (x_right,
 * x_down), a step along physical y by (y_right, y_down). */
typedef struct {
    int x_right, x_down, y_right, y_down;
} r3d_quarter_axes_t;

static inline r3d_quarter_axes_t
r3d_quarter_axes(int quarter) {
    switch (quarter & 3) {
        case 1: return (r3d_quarter_axes_t){0, -1, 1, 0};
        case 2: return (r3d_quarter_axes_t){-1, 0, 0, -1};
        case 3: return (r3d_quarter_axes_t){0, 1, -1, 0};
        default: return (r3d_quarter_axes_t){1, 0, 0, 1};
    }
}

/* The inverse of ui_transform_quarter_turn(): where in the upright picture
 * a physical pixel lands once the panel is read at `viewport.quarter`.
 * Spelled out here rather than included, since render/ sits below ui/. */
static inline void
r3d_physical_to_upright(r3d_viewport_t viewport, int px, int py, int* ux, int* uy) {
    const r3d_quarter_axes_t a = r3d_quarter_axes(viewport.quarter);
    const bool swapped = a.x_right == 0;
    const int upright_width = swapped ? viewport.height : viewport.width;
    const int upright_height = swapped ? viewport.width : viewport.height;
    *ux = (a.x_right + a.y_right < 0 ? upright_width - 1 : 0) + a.x_right * px + a.y_right * py;
    *uy = (a.x_down + a.y_down < 0 ? upright_height - 1 : 0) + a.x_down * px + a.y_down * py;
}

/* The normalised direction for physical pixel (px, py): upright mapping,
 * then a lens fit to the upright viewport's SHORTER axis. */
static inline r3d_vec3f_t
r3d_ray_direction(const r3d_ray_camera_t* cam, int px, int py) {
    int ux, uy;
    r3d_physical_to_upright(cam->viewport, px, py, &ux, &uy);

    const bool swapped = (cam->viewport.quarter & 1) != 0;
    const int eff_width = swapped ? cam->viewport.height : cam->viewport.width;
    const int eff_height = swapped ? cam->viewport.width : cam->viewport.height;

    const float ndc_x = ((float)ux + 0.5f) / (float)eff_width * 2.0f - 1.0f;
    const float ndc_y = 1.0f - ((float)uy + 0.5f) / (float)eff_height * 2.0f;
    const float aspect = (float)eff_width / (float)eff_height;
    const float half_w = aspect >= 1.0f ? cam->half_fov_short_tan * aspect : cam->half_fov_short_tan;
    const float half_h = aspect >= 1.0f ? cam->half_fov_short_tan : cam->half_fov_short_tan / aspect;

    r3d_vec3f_t dir = cam->forward;
    dir = r3d_vec3f_add(dir, r3d_vec3f_scale(cam->right, ndc_x * half_w));
    dir = r3d_vec3f_add(dir, r3d_vec3f_scale(cam->up, ndc_y * half_h));
    return r3d_vec3f_normalize(dir);
}
