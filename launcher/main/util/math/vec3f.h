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
