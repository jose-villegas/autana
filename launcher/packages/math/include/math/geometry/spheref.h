/* spheref: a closed sphere of a float. */
#pragma once
#include "math/geometry/box3f.h"
#include "math/geometry/sphere_template.h"
#include "math/linear/vec_convert.h"

MATH_DEFINE_SPHERE(spheref, vec3f, vec3f, MATH_WIDEN_NONE, box3f, float, mathf)
