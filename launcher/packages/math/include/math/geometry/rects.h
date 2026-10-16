/*
 * rects: a half-open rectangle of an int16. The operations are
 * rect_template.h's; docs/math/README.md says how they are made.
 */
#pragma once

#include <stdint.h>

#include "math/geometry/rect_template.h"
#include "math/scalar/maths.h"

MATH_DEFINE_RECT(rects, int16_t, maths)
