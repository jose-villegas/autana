/* spheres: a closed sphere of int16 position ticks. */
#pragma once
#include <stdint.h>
#include "math/geometry/ball_template.h"
#include "math/linear/vec3i.h"
#include "math/linear/vec3s.h"
#include "math/linear/vec_convert.h"
#include "math/scalar/maths.h"

MATH_DEFINE_BALL(spheres, vec3s, vec3i, vec3i_from_vec3s, int16_t, maths)
