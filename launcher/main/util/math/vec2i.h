/*
 * vec2i: a 2-component vector of an int32. The operations are
 * vec2_template.h's; math_template.h says how they are made.
 */
#pragma once

#include <stdint.h>

#include "util/math/vec2_template.h"
#include "util/scalar/mathi.h"

MATH_DEFINE_VEC2(vec2i, int32_t, int64_t, mathi)
