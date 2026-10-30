/* box3f_plane: a float box against a float plane (box3_plane_template.h). */
#pragma once

#include "math/geometry/box3_plane_template.h"
#include "math/geometry/box3f.h"
#include "math/geometry/planef.h"

MATH_DEFINE_BOX3_PLANE(box3f, vec3f, planef, float, mathf)
