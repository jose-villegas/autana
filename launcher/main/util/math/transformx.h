/*
 * transformx: a transform of a Q16.16: position, rotation, scale and the
 * cached model matrix. The operations, which matrix call to use when, and
 * the axes are transform_template.h's; math_template.h says how they are
 * made.
 */
#pragma once

#include "util/math/mat4x.h"
#include "util/math/quatx.h"
#include "util/math/transform_template.h"
#include "util/math/vec3x.h"
#include "util/scalar/mathx.h"

MATH_DEFINE_TRANSFORM(transformx, vec3x, quatx, mat4x, mathx)

/* An initializer: `transformx_t t = TRANSFORMX_IDENTITY;`, or
 * `t = (transformx_t)TRANSFORMX_IDENTITY;`. Zero-initialized does not
 * mean identity: its scale is zero, so its matrix sends every point to the
 * origin. */
#define TRANSFORMX_IDENTITY                                                                                            \
    {.position = {0, 0, 0},                                                                                            \
     .rotation = {0, 0, 0, MATHX_ONE},                                                                                 \
     .scale = {MATHX_ONE, MATHX_ONE, MATHX_ONE},                                                                       \
     .matrix = {{{MATHX_ONE, 0, 0, 0}, {0, MATHX_ONE, 0, 0}, {0, 0, MATHX_ONE, 0}, {0, 0, 0, MATHX_ONE}}},             \
     .cached = true}
