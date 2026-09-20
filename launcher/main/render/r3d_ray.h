/*
 * r3d_ray - a float ray camera: origin plus an orthonormal forward/right/up
 * basis, and the physical-pixel-to-direction math a ray tracer needs. This
 * is what rt_cornell.c's physical_to_logical()/camera_ray_dir() did by
 * hand: the quarter-turn physical-to-upright mapping and fitting the lens
 * to a viewport's SHORTER axis, both shared with r3d_viewport_t.
 *
 * Single-precision throughout on purpose - a ray camera's pose is not
 * exactly representable in S3L_F units - so a `.c` including this carries
 * `#pragma GCC diagnostic error "-Wdouble-promotion"` itself. No converter
 * to or from the fixed-point r3d_camera_t: nothing calls one yet.
 */
#pragma once

#include <math.h>
#include <stdbool.h>

#include "render/r3d_camera.h"

typedef struct {
    float x, y, z;
} r3d_vec3f_t;

static inline r3d_vec3f_t
r3d_vec3f_add(r3d_vec3f_t a, r3d_vec3f_t b) {
    return (r3d_vec3f_t){a.x + b.x, a.y + b.y, a.z + b.z};
}

static inline r3d_vec3f_t
r3d_vec3f_sub(r3d_vec3f_t a, r3d_vec3f_t b) {
    return (r3d_vec3f_t){a.x - b.x, a.y - b.y, a.z - b.z};
}

static inline r3d_vec3f_t
r3d_vec3f_scale(r3d_vec3f_t a, float s) {
    return (r3d_vec3f_t){a.x * s, a.y * s, a.z * s};
}

static inline float
r3d_vec3f_dot(r3d_vec3f_t a, r3d_vec3f_t b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

static inline r3d_vec3f_t
r3d_vec3f_normalize(r3d_vec3f_t a) {
    return r3d_vec3f_scale(a, 1.0f / sqrtf(r3d_vec3f_dot(a, a)));
}

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

/* The inverse of the rotation ui_transform_quarter_turn()/write_bmp() apply
 * when reading a physical canvas back out at `viewport.quarter` - worked out
 * by hand for the four cases rather than pulled in as a UI-layer dependency. */
static inline void
r3d_physical_to_upright(r3d_viewport_t viewport, int px, int py, int* ux, int* uy) {
    switch (viewport.quarter) {
        case 1:
            *ux = py;
            *uy = viewport.width - 1 - px;
            break;
        case 2:
            *ux = viewport.width - 1 - px;
            *uy = viewport.height - 1 - py;
            break;
        case 3:
            *ux = viewport.height - 1 - py;
            *uy = px;
            break;
        default:
            *ux = px;
            *uy = py;
            break;
    }
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
