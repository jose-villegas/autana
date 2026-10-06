/*
 * vec3s: a 3-component vector of an int16. The operations are
 * vec3_template.h's and vec_swizzle_template.h's; math_template.h says how
 * they are made.
 */
#pragma once

#include <stdint.h>

#include "util/math/vec2s.h"
#include "util/math/vec3_template.h"
#include "util/scalar/maths.h"

MATH_DEFINE_VEC3(vec3s, int16_t, int64_t, maths)
MATH_DEFINE_VEC_SWIZZLE(vec3s, vec2s, int16_t)
