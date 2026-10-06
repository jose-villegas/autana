/*
 * transformf: a transform of a float: position, rotation, scale and the
 * cached model matrix. The operations, which matrix call to use when, and
 * the axes are transform_template.h's; math_template.h says how they are
 * made.
 */
#pragma once

#include "util/math/mat4f.h"
#include "util/math/quatf.h"
#include "util/math/transform_template.h"
#include "util/math/vec3f.h"
#include "util/scalar/mathf.h"

MATH_DEFINE_TRANSFORM(transformf, vec3f, quatf, mat4f, mathf)

/* An initializer: `transformf_t t = TRANSFORMF_IDENTITY;`, or
 * `t = (transformf_t)TRANSFORMF_IDENTITY;`. Zero-initialized does not
 * mean identity: its scale is zero, so its matrix sends every point to the
 * origin. */
#define TRANSFORMF_IDENTITY                                                                                            \
    {.position = {0.0F, 0.0F, 0.0F},                                                                                   \
     .rotation = {0.0F, 0.0F, 0.0F, 1.0F},                                                                             \
     .scale = {1.0F, 1.0F, 1.0F},                                                                                      \
     .matrix =                                                                                                         \
         {{{1.0F, 0.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 1.0F, 0.0F}, {0.0F, 0.0F, 0.0F, 1.0F}}},   \
     .cached = true}
