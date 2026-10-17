/*
 * projectx: near-plane segment clipping in any fixed camera-space scale.
 * Endpoints and near_z share a scale; the crossing ratio is Q16.16.
 * Points at or behind the near plane are excluded.
 */
#pragma once

#include <stdbool.h>

#include "math/linear/project_template.h"
#include "math/linear/vec3x.h"
#include "math/scalar/mathx.h"

static inline void
projectx_cross_near(int32_t near_z, vec3x_t* behind, const vec3x_t* front) {
    const int64_t frac_q16 = ((int64_t)(near_z - behind->z) * MATHX_ONE) / (front->z - behind->z);

    behind->x += (int32_t)(((int64_t)(front->x - behind->x) * frac_q16) >> MATHX_SHIFT);
    behind->y += (int32_t)(((int64_t)(front->y - behind->y) * frac_q16) >> MATHX_SHIFT);
    behind->z = near_z;
}

MATH_DEFINE_CLIP_NEAR(projectx, vec3x_t, int32_t)
