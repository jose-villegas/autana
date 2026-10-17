/*
 * projectf: pinhole projection in pixels with camera +y up and picture y down.
 * Focal lengths are pixels per camera unit at unit depth; the optical centre
 * is in pixels and the near plane in camera units. Clipping excludes points
 * at or behind the near plane.
 */
#pragma once

#include <stdbool.h>

#include "math/linear/project_template.h"
#include "math/linear/vec2f.h"
#include "math/linear/vec3f.h"
#include "math/scalar/mathf.h"

typedef struct {
    float fx, fy, cx, cy, near_z;
} projectf_t;

/* The projected offset from the optical centre in pixels, camera +y up (not yet flipped to picture y). */
static inline vec2f_t
projectf_offset(const projectf_t* lens, vec3f_t p) {
    const float reciprocal_z = mathf_recip(p.z);
    const float gain_x = lens->fx * reciprocal_z;
    const float gain_y = lens->fy * reciprocal_z;
    return (vec2f_t){p.x * gain_x, p.y * gain_y};
}

static inline vec2f_t
projectf_screen(const projectf_t* lens, vec3f_t p) {
    const vec2f_t offset = projectf_offset(lens, p);
    return (vec2f_t){lens->cx + offset.x, lens->cy - offset.y};
}

static inline void
_projectf_cross_near(float near_z, vec3f_t* behind, const vec3f_t* front) {
    const float frac = (near_z - behind->z) / (front->z - behind->z);

    behind->x += (front->x - behind->x) * frac;
    behind->y += (front->y - behind->y) * frac;
    behind->z = near_z;
}

MATH_DEFINE_CLIP_NEAR(projectf, vec3f_t, float, _projectf_cross_near)
