/*
 * vec2f: a 2-component vector of a float. The operations are
 * vec2_template.h's; math_template.h says how they are made.
 */
#pragma once

#include <stdint.h>

#include "util/math/mathf.h"
#include "util/math/vec2_template.h"

MATH_DEFINE_VEC2(vec2f, float, float, mathf)
