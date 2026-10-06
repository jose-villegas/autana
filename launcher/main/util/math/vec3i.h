/*
 * vec3i: a 3-component vector of an int32. The operations are
 * vec3_template.h's and vec_swizzle_template.h's; math_template.h says how
 * they are made.
 */
#pragma once

#include <stdint.h>

#include "util/math/vec2i.h"
#include "util/math/vec3_template.h"
#include "util/scalar/mathi.h"

MATH_DEFINE_VEC3(vec3i, int32_t, int64_t, mathi)
MATH_DEFINE_VEC_SWIZZLE(vec3i, vec2i, int32_t)
