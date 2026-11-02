/* spheres: a closed sphere of int16 position ticks. */
#pragma once
#include <stdint.h>
#include "math/geometry/ball_template.h"
#include "math/linear/vec3s.h"
#include "math/scalar/maths.h"

MATH_DEFINE_BALL(spheres, vec3s, int16_t, maths)
