/*
 * projectx: Q16.16 pinhole lens and near clipping in any fixed camera-space scale.
 * Endpoints and near_z share a scale; the crossing ratio is Q16.16.
 * Points at or behind the near plane are excluded.
 */
#pragma once

#include <stdbool.h>

#include "math/linear/vec3x.h"
#include "math/projection/project_template.h"
#include "math/projection/projectf.h"
#include "math/scalar/mathx.h"

/* Q16.16 pinhole values, camera +y up and picture y down. */
typedef struct {
    int32_t fx, fy, cx, cy, near_z;
} projectx_t;

static inline projectx_t
projectx_from_projectf(const projectf_t* lens) {
    return (projectx_t){mathf_to_x(lens->fx), mathf_to_x(lens->fy), mathf_to_x(lens->cx), mathf_to_x(lens->cy),
                        mathf_to_x(lens->near_z)};
}

static inline void
_projectx_cross_near(int32_t near_z, vec3x_t* behind, const vec3x_t* front) {
    /* In [0, MATHX_ONE): exactly one endpoint is behind the plane. */
    const int32_t frac_q16 = (int32_t)(((int64_t)(near_z - behind->z) * MATHX_ONE) / (front->z - behind->z));

    behind->x += fx_mul_floor(front->x - behind->x, frac_q16, MATHX_SHIFT);
    behind->y += fx_mul_floor(front->y - behind->y, frac_q16, MATHX_SHIFT);
    behind->z = near_z;
}

MATH_DEFINE_CLIP_NEAR(projectx, vec3x_t, int32_t, _projectx_cross_near)
