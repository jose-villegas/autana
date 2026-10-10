/*
 * mat4x: a 4x4 matrix of a Q16.16. The operations are mat4_template.h's;
 * math_template.h says how they are made.
 */
#pragma once

#include "math/linear/mat4_template.h"
#include "math/linear/quatx.h"
#include "math/linear/vec3x.h"
#include "math/scalar/mathx.h"

MATH_DEFINE_MAT4(mat4x, vec3x, quatx, int32_t, mathx)
