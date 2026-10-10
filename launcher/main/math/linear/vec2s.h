/*
 * vec2s: a 2-component vector of an int16. The operations are
 * vec2_template.h's; math_template.h says how they are made.
 */
#pragma once

#include <stdint.h>

#include "math/linear/vec2_template.h"
#include "math/scalar/maths.h"

MATH_DEFINE_VEC2(vec2s, int16_t, int64_t, maths)
