/* planex: an oriented plane of a Q16.16. */
#pragma once

#include <stdint.h>

#include "math/geometry/plane_template.h"
#include "math/geometry/rectx.h"
#include "math/linear/mat4x.h"
#include "math/linear/vec2x.h"
#include "math/linear/vec3x.h"
#include "math/scalar/mathx.h"

MATH_DEFINE_PLANE(planex, vec3x, vec2x, rectx, mat4x, int32_t, mathx)
