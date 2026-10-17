/*
 * projectx: near-plane segment clipping in any fixed camera-space scale.
 * Endpoints and near_z share a scale; the crossing ratio is Q16.16.
 * Points at or behind the near plane are excluded.
 */
#pragma once

#include <stdbool.h>

#include "math/linear/vec3x.h"
#include "math/scalar/mathx.h"

static inline bool
projectx_clip_near(int32_t near_z, vec3x_t* p0, vec3x_t* p1) {
    const bool front0 = p0->z > near_z;
    const bool front1 = p1->z > near_z;

    if (!front0 && !front1) {
        return false;
    }

    if (front0 != front1) {
        vec3x_t* behind = front0 ? p1 : p0;
        const vec3x_t* front = front0 ? p0 : p1;
        const int64_t frac_q16 = ((int64_t)(near_z - behind->z) * MATHX_ONE) / (front->z - behind->z);

        behind->x += (int32_t)(((int64_t)(front->x - behind->x) * frac_q16) >> MATHX_SHIFT);
        behind->y += (int32_t)(((int64_t)(front->y - behind->y) * frac_q16) >> MATHX_SHIFT);
        behind->z = near_z;
    }

    return true;
}
