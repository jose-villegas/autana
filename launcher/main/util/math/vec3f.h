/*
 * vec3f: a 3-component vector of a float. The operations are
 * vec3_template.h's and vec_swizzle_template.h's; math_template.h says how
 * they are made.
 */
#pragma once

#include <stdint.h>

#include "util/math/mathf.h"
#include "util/math/vec2f.h"
#include "util/math/vec3_template.h"

MATH_DEFINE_VEC3(vec3f, float, float, mathf)
MATH_DEFINE_VEC3_NORMALIZE(vec3f, float, mathf)
MATH_DEFINE_VEC_SWIZZLE(vec3f, vec2f, float)

/* The octahedral map (Cigolle et al. 2014): the point of the square [-1, 1]^2
 * that the direction `n` lands on when projected onto the octahedron
 * |x| + |y| + |z| = 1, its z < 0 half folded out over the diagonals. Any length
 * but zero gives the same point, so the input need not be normalized; one
 * divide. */
static inline vec2f_t
vec3f_octahedral(vec3f_t n) {
    const float inv = 1.0F / (fabsf(n.x) + fabsf(n.y) + fabsf(n.z));
    const float u = n.x * inv;
    const float v = n.y * inv;
    if (n.z >= 0.0F) {
        return (vec2f_t){u, v};
    }
    return (vec2f_t){copysignf(1.0F - fabsf(v), u), copysignf(1.0F - fabsf(u), v)};
}

/* The unit direction a point of [-1, 1]^2 stands for: vec3f_octahedral's
 * inverse. */
static inline vec3f_t
vec3f_from_octahedral(vec2f_t p) {
    const float z = 1.0F - fabsf(p.x) - fabsf(p.y);
    if (z >= 0.0F) {
        return vec3f_normalize((vec3f_t){p.x, p.y, z});
    }
    return vec3f_normalize((vec3f_t){copysignf(1.0F - fabsf(p.y), p.x), copysignf(1.0F - fabsf(p.x), p.y), z});
}
