/* inverse_depth: depth stored as code = scale / z * code_max, for z >= scale,
 * which steps linearly across the screen. Its precision per unit of depth and
 * the scale that keeps the nearest thing seen within the code. */
#pragma once

#include <math.h>

#include "math/scalar/mathf.h"

/* Codes per unit of depth at z: code_max scale / z^2. */
static inline float
inverse_depth_steps_per_unit(float scale, float code_max, float z) {
    return code_max * scale / (z * z);
}

/* The scale for a picture whose nearest point is at nearest_z or beyond:
 * nearest_z down to a power of two (its exponent alone, so a bound that
 * drifts by rounding keeps one scale), never under the clip plane near_z,
 * and near_z itself when nothing is past it. */
static inline float
inverse_depth_scale_fit(float near_z, float nearest_z) {
    if (!(nearest_z > near_z)) {
        return near_z;
    }
    int exponent;
    (void)frexpf(nearest_z, &exponent);
    return mathf_max(near_z, ldexpf(0.5F, exponent));
}
