/*
 * transformf: a transform of a float: position, rotation, scale and the
 * cached model matrix. The operations, which matrix call to use when, and
 * the axes are transform_template.h's; docs/math/Linear.md says how they are
 * made.
 */
#pragma once

#include "math/linear/mat4f.h"
#include "math/linear/quatf.h"
#include "math/linear/transform_template.h"
#include "math/linear/vec3f.h"
#include "math/scalar/mathf.h"

MATH_DEFINE_TRANSFORM(transformf, vec3f, quatf, mat4f, mathf)

/* An initializer: `transformf_t t = TRANSFORMF_IDENTITY;`, or
 * `t = (transformf_t)TRANSFORMF_IDENTITY;`. Zero-initialized does not
 * mean identity: its scale is zero, so its matrix sends every point to the
 * origin. */
#define TRANSFORMF_IDENTITY                                                                                            \
    {.position = {0.0F, 0.0F, 0.0F},                                                                                   \
     .rotation = {0.0F, 0.0F, 0.0F, 1.0F},                                                                             \
     .scale = {1.0F, 1.0F, 1.0F},                                                                                      \
     .matrix = MAT4F_IDENTITY,                                                                                         \
     .cached = true}

/* Turns `t` by `angle` radians about the line through `point` along `axis`
 * (any length but zero), in the parent's frame. Its position swings round
 * `point` and its rotation turns with it, so whatever it faced it still
 * faces, turned. */
static inline void
transformf_rotate_around(transformf_t* t, vec3f_t point, vec3f_t axis, float angle) {
    const quatf_t turn = quatf_from_axis_angle(vec3f_normalize(axis), angle);
    transformf_set_position(t, vec3f_add(point, quatf_rotate(turn, vec3f_sub(t->position, point))));
    transformf_set_rotation(t, quatf_normalize(quatf_mul(turn, t->rotation)));
}

#define TRANSFORMF_MATRIX_TOLERANCE 1e-4F

/* Decomposes affine TRS; refuses shear, reflection, singular or nonfinite matrices. */
static inline bool
transformf_from_mat(mat4f_t matrix, transformf_t* out) {
    vec3f_t axes[3];
    float scale[3];
    for (int c = 0; c < 3; c++) {
        axes[c] = (vec3f_t){matrix.m[0][c], matrix.m[1][c], matrix.m[2][c]};
        scale[c] = sqrtf(vec3f_dot(axes[c], axes[c]));
        if (!isfinite(scale[c]) || scale[c] <= 0.0F) {
            return false;
        }
        axes[c] = vec3f_scale(axes[c], 1.0F / scale[c]);
        if ((!isfinite(matrix.m[3][c]) || fabsf(matrix.m[3][c]) > TRANSFORMF_MATRIX_TOLERANCE)
            || !isfinite(matrix.m[c][3])) {
            return false;
        }
    }
    if (!isfinite(matrix.m[3][3]) || fabsf(matrix.m[3][3] - 1.0F) > TRANSFORMF_MATRIX_TOLERANCE) {
        return false;
    }
    if (fabsf(vec3f_dot(axes[0], axes[1])) > TRANSFORMF_MATRIX_TOLERANCE
        || fabsf(vec3f_dot(axes[0], axes[2])) > TRANSFORMF_MATRIX_TOLERANCE
        || fabsf(vec3f_dot(axes[1], axes[2])) > TRANSFORMF_MATRIX_TOLERANCE
        || vec3f_dot(vec3f_cross(axes[0], axes[1]), axes[2]) <= 0.0F) {
        return false;
    }
    *out = (transformf_t){.position = {matrix.m[0][3], matrix.m[1][3], matrix.m[2][3]},
                          .rotation = quatf_normalize(quatf_from_basis(axes[0], axes[1], axes[2])),
                          .scale = {scale[0], scale[1], scale[2]},
                          .matrix = matrix,
                          .cached = true};
    return true;
}
