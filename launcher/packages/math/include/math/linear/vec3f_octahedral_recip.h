/* vec3f_octahedral_recip: octahedral projection using mathf_recip for a nonzero direction. */
#pragma once
#include "math/linear/vec3f.h"

static inline __attribute__((always_inline)) vec2f_t
vec3f_octahedral_recip(vec3f_t normal) {
    const float inv = mathf_recip(fabsf(normal.x) + fabsf(normal.y) + fabsf(normal.z));
    const vec2f_t point = {normal.x * inv, normal.y * inv};
    if (normal.z >= 0.0F) {
        return point;
    }
    return (vec2f_t){copysignf(1.0F - fabsf(point.y), point.x), copysignf(1.0F - fabsf(point.x), point.y)};
}
