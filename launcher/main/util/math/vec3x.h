/*
 * vec3x: a 3-component vector of a Q16.16. The operations are
 * vec3_template.h's; math_template.h says how they are made.
 */
#pragma once

#include <stdint.h>

#include "util/math/mathx.h"
#include "util/math/vec3_template.h"

MATH_DEFINE_VEC3(vec3x, int32_t, int32_t, mathx)
MATH_DEFINE_VEC3_NORMALIZE(vec3x, int32_t, mathx)
