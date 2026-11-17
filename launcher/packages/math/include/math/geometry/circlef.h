/* circlef: a closed disc of a float. */
#pragma once
#include "math/geometry/circle_template.h"
#include "math/geometry/rectf.h"
#include "math/linear/vec2f.h"
#include "math/linear/vec_convert.h"

MATH_DEFINE_CIRCLE(circlef, vec2f, vec2f, MATH_WIDEN_NONE, rectf, float, mathf, mathf_zero())
MATH_DEFINE_CIRCLE_REAL(circlef, vec2f, rectf, float, mathf)
