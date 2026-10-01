/*
 * vec2x: a 2-component vector of a Q16.16. The operations are
 * vec2_template.h's; math_template.h says how they are made.
 */
#pragma once

#include <stdint.h>

#include "util/math/mathx.h"
#include "util/math/vec2_template.h"

MATH_DEFINE_VEC2(vec2x, int32_t, int32_t, mathx)
