/*
 * projectf: pinhole projection in pixels with camera +y up and picture y down.
 * Focal lengths are pixels per camera unit at unit depth; the optical centre
 * is in pixels and the near plane in camera units. Clipping excludes points
 * at or behind the near plane.
 */
#pragma once

#include <stdbool.h>

#include "math/linear/vec2f.h"
#include "math/linear/vec3f.h"
#include "math/scalar/mathf.h"

typedef struct {
    float fx, fy, cx, cy, near_z;
} projectf_t;

static inline vec2f_t
projectf_screen(const projectf_t* lens, vec3f_t p) {
    const float gain_x = lens->fx * mathf_recip(p.z);
    const float gain_y = lens->fy * mathf_recip(p.z);
    return (vec2f_t){lens->cx + p.x * gain_x, lens->cy - p.y * gain_y};
}

static inline bool
projectf_clip_near(float near_z, vec3f_t* p0, vec3f_t* p1) {
    const bool front0 = p0->z > near_z;
    const bool front1 = p1->z > near_z;

    if (!front0 && !front1) {
        return false;
    }

    if (front0 != front1) {
        vec3f_t* behind = front0 ? p1 : p0;
        const vec3f_t* front = front0 ? p0 : p1;
        const float frac = (near_z - behind->z) / (front->z - behind->z);

        behind->x += (front->x - behind->x) * frac;
        behind->y += (front->y - behind->y) * frac;
        behind->z = near_z;
    }

    return true;
}
