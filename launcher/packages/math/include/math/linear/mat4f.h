/*
 * mat4f: a 4x4 matrix of a float. The operations are mat4_template.h's;
 * math_template.h says how they are made.
 */
#pragma once

#include "math/linear/mat4_template.h"
#include "math/linear/quatf.h"
#include "math/linear/vec3f.h"
#include "math/scalar/mathf.h"

MATH_DEFINE_MAT4(mat4f, vec3f, quatf, float, mathf)
MATH_DEFINE_MAT4_INVERT_AFFINE(mat4f, float, mathf)
