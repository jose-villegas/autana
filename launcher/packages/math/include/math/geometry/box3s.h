/* box3s: a closed axis-aligned box of int16 position ticks. */
#pragma once
#include <stdint.h>
#include "math/geometry/box3_template.h"
#include "math/linear/vec3s.h"
#include "math/scalar/maths.h"

MATH_DEFINE_BOX3(box3s, vec3s, int16_t, maths, INT16_MIN, INT16_MAX)
