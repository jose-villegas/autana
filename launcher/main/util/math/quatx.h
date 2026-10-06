/*
 * quatx: a Q16.16 rotation; angles are turns (mathx.h). It has no slerp: that
 * needs an arccosine the fixed-point type does not carry. The operations are
 * quat_template.h's; math_template.h says how they are made.
 */
#pragma once

#include "util/math/quat_template.h"
#include "util/math/vec3x.h"
#include "util/scalar/mathx.h"

MATH_DEFINE_QUAT(quatx, vec3x, int32_t, mathx)
MATH_DEFINE_QUAT_NORMALIZE(quatx, vec3x, int32_t, mathx)
