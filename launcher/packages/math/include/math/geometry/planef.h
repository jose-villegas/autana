/* planef: an oriented plane of a float. */
#pragma once

#include "math/geometry/plane_template.h"
#include "math/geometry/rectf.h"
#include "math/linear/mat4f.h"
#include "math/linear/vec3f.h"
#include "math/projection/projectf.h"
#include "math/scalar/mathf.h"

MATH_DEFINE_PLANE(planef, vec3f, rectf, mat4f, projectf, float, mathf)
