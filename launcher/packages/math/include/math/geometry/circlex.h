/* circlex: a closed disc of a Q16.16. */
#pragma once
#include "math/geometry/circle_template.h"
#include "math/geometry/rectx.h"
#include "math/linear/vec2x.h"

MATH_DEFINE_CIRCLE(circlex, vec2x, rectx, int32_t, mathx, mathx_zero())
MATH_DEFINE_CIRCLE_REAL(circlex, vec2x, rectx, int32_t, mathx)
