/*
 * rectx: a half-open rectangle of a Q16.16. The operations are
 * rect_template.h's; docs/math/Geometry.md says how they are made.
 */
#pragma once

#include <stdint.h>

#include "math/geometry/rect_template.h"
#include "math/scalar/mathx.h"

MATH_DEFINE_RECT(rectx, int32_t, mathx)
