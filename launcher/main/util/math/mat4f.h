/*
 * mat4f: a 4x4 matrix of a float. The operations are mat4_template.h's;
 * math_template.h says how they are made.
 */
#pragma once

#include "util/math/mat4_template.h"
#include "util/math/quatf.h"
#include "util/math/vec3f.h"
#include "util/scalar/mathf.h"

MATH_DEFINE_MAT4(mat4f, vec3f, quatf, float, mathf)
MATH_DEFINE_MAT4_INVERT_AFFINE(mat4f, float, mathf)
