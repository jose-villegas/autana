/*
 * vec2i: a 2-component vector of an int32. The operations are
 * vec2_template.h's; docs/math/Linear.md says how they are made.
 */
#pragma once

#include <stdint.h>

#include "math/linear/vec2_template.h"
#include "math/scalar/mathi.h"

MATH_DEFINE_VEC2(vec2i, int32_t, int64_t, mathi)
MATH_DEFINE_VEC_LERP_RATIO(vec2i, mathi, 2)
