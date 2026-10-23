/*
 * vec3x: a 3-component vector of a Q16.16. The operations are
 * vec3_template.h's and vec_swizzle_template.h's; docs/math/Linear.md says how
 * they are made.
 */
#pragma once

#include <stdint.h>

#include "math/linear/vec2x.h"
#include "math/linear/vec3_template.h"
#include "math/scalar/mathx.h"

MATH_DEFINE_VEC3(vec3x, int32_t, int32_t, mathx)
MATH_DEFINE_VEC_LERP(vec3x, int32_t, mathx, 3)
MATH_DEFINE_VEC_SWIZZLE(vec3x, vec2x, int32_t)

/* out = a/|a|; *q is within relative 2^-28 plus 0.5 LSB of t/|a|
 * clamped to the int32 range. A zero vector gives zero and mathx_div(t, 0). */
static inline vec3x_t
vec3x_normalize_divide(vec3x_t a, int32_t t, int32_t* q) {
    const int32_t in[] = {a.x, a.y, a.z};
    int32_t out[3];
    mathx_normalize_n(in, out, 3, t, q);
    return (vec3x_t){out[0], out[1], out[2]};
}

/* Unit vector; a zero vector gives zero. */
static inline vec3x_t
vec3x_normalize(vec3x_t a) {
    int32_t quotient;
    return vec3x_normalize_divide(a, 0, &quotient);
}
