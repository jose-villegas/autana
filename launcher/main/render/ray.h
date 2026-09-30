/*
 * ray: the ray tracer's camera, an origin and an orthonormal
 * forward/right/up basis, and the direction through each physical pixel of
 * a viewport_t, upright for its quarter, with the lens fitted to the
 * viewport's SHORTER axis.
 *
 * Single precision only, for the reason vec3f.h gives. The pose is
 * float rather than S3L_F units because a caller's numbers need not be
 * representable there.
 */
#pragma once

#include <stdbool.h>

#include "render/vec3f.h"
#include "render/viewport.h"

typedef struct {
    vec3f_t origin, forward, right, up;
    float half_fov_short_tan;
    viewport_t viewport;
} ray_camera_t;

static inline void
ray_camera_init(ray_camera_t* cam, vec3f_t origin, vec3f_t forward, vec3f_t right, vec3f_t up, float half_fov_short_tan,
                viewport_t viewport) {
    cam->origin = origin;
    cam->forward = forward;
    cam->right = right;
    cam->up = up;
    cam->half_fov_short_tan = half_fov_short_tan;
    cam->viewport = viewport;
}

/* The normalised direction for physical pixel (px, py): upright mapping,
 * then a lens fit to the upright viewport's SHORTER axis. */
static inline vec3f_t
ray_direction(const ray_camera_t* cam, int px, int py) {
    int ux, uy;
    viewport_physical_to_upright(cam->viewport, px, py, &ux, &uy);

    const bool swapped = (cam->viewport.quarter & 1) != 0;
    const int eff_width = swapped ? cam->viewport.height : cam->viewport.width;
    const int eff_height = swapped ? cam->viewport.width : cam->viewport.height;

    const float ndc_x = ((float)ux + 0.5f) / (float)eff_width * 2.0f - 1.0f;
    const float ndc_y = 1.0f - ((float)uy + 0.5f) / (float)eff_height * 2.0f;
    const float aspect = (float)eff_width / (float)eff_height;
    const float half_w = aspect >= 1.0f ? cam->half_fov_short_tan * aspect : cam->half_fov_short_tan;
    const float half_h = aspect >= 1.0f ? cam->half_fov_short_tan : cam->half_fov_short_tan / aspect;

    vec3f_t dir = cam->forward;
    dir = vec3f_add(dir, vec3f_scale(cam->right, ndc_x * half_w));
    dir = vec3f_add(dir, vec3f_scale(cam->up, ndc_y * half_h));
    return vec3f_normalize(dir);
}
