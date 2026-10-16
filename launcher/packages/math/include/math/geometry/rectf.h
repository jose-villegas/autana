/*
 * rectf: a half-open rectangle of a float. The operations are
 * rect_template.h's; docs/math/README.md says how they are made.
 */
#pragma once

#include "math/geometry/rect_template.h"
#include "math/scalar/mathf.h"

MATH_DEFINE_RECT(rectf, float, mathf)
