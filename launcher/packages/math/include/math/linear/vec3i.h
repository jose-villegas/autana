/*
 * vec3i: a 3-component vector of an int32. The operations are
 * vec3_template.h's and vec_swizzle_template.h's; docs/math/README.md says how
 * they are made.
 */
#pragma once

#include <stdint.h>

#include "math/linear/vec2i.h"
#include "math/linear/vec3_template.h"
#include "math/scalar/mathi.h"

MATH_DEFINE_VEC3(vec3i, int32_t, int64_t, mathi)
MATH_DEFINE_VEC_LERP_RATIO(vec3i, mathi, 3)
MATH_DEFINE_VEC_SWIZZLE(vec3i, vec2i, int32_t)
