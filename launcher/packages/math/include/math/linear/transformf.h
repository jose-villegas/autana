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

/* `child` placed in `parent`'s frame: where the child stands in the frame
 * the parent stands in. Exact when the parent's scale is even (see
 * transformf_scale_is_even); an unevenly scaled parent holding a turned child
 * shears, which no TRS holds, and the scale is then per axis. */
static inline transformf_t
transformf_compose(const transformf_t* parent, const transformf_t* child) {
    const vec3f_t scaled = {parent->scale.x * child->position.x, parent->scale.y * child->position.y,
                            parent->scale.z * child->position.z};
    return (transformf_t){
        .position = vec3f_add(parent->position, quatf_rotate(parent->rotation, scaled)),
        .rotation = quatf_normalize(quatf_mul(parent->rotation, child->rotation)),
        .scale = {parent->scale.x * child->scale.x, parent->scale.y * child->scale.y, parent->scale.z * child->scale.z},
        .cached = false};
}

#define TRANSFORMF_MATRIX_TOLERANCE 1e-4F

/* True when the three scales agree within TRANSFORMF_MATRIX_TOLERANCE of the
 * largest: a parent scaled so composes into TRS exactly. */
static inline bool
transformf_scale_is_even(const transformf_t* t) {
    const float most = fmaxf(t->scale.x, fmaxf(t->scale.y, t->scale.z));
    const float least = fminf(t->scale.x, fminf(t->scale.y, t->scale.z));
    const float largest = fmaxf(fabsf(t->scale.x), fmaxf(fabsf(t->scale.y), fabsf(t->scale.z)));
    return most - least <= TRANSFORMF_MATRIX_TOLERANCE * largest;
}

/* True when the transform turns the space inside out: an odd count of
 * negative scales, as transformf_from_mat gives a reflection. */
static inline bool
transformf_mirrors(const transformf_t* t) {
    return t->scale.x * t->scale.y * t->scale.z < 0.0F;
}

/* The inverse of transformf_compose: the child that, placed in `parent`,
 * stands at `world`. `parent`'s scale must be even and nonzero. */
static inline transformf_t
transformf_relative(const transformf_t* parent, const transformf_t* world) {
    const quatf_t turn = parent->rotation;
    const quatf_t back = {-turn.x, -turn.y, -turn.z, turn.w}; /* the inverse of a unit rotation */
    const float inverse_scale = 1.0F / parent->scale.x;
    return (transformf_t){
        .position = vec3f_scale(quatf_rotate(back, vec3f_sub(world->position, parent->position)), inverse_scale),
        .rotation = quatf_normalize(quatf_mul(back, world->rotation)),
        .scale = vec3f_scale(world->scale, inverse_scale),
        .cached = false};
}

/* Decomposes affine TRS; a reflection comes back as a negative scale.x.
 * Refuses shear, singular or nonfinite matrices. */
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
        || fabsf(vec3f_dot(axes[1], axes[2])) > TRANSFORMF_MATRIX_TOLERANCE) {
        return false;
    }
    if (vec3f_dot(vec3f_cross(axes[0], axes[1]), axes[2]) < 0.0F) {
        axes[0] = vec3f_scale(axes[0], -1.0F);
        scale[0] = -scale[0];
    }
    *out = (transformf_t){.position = {matrix.m[0][3], matrix.m[1][3], matrix.m[2][3]},
                          .rotation = quatf_normalize(quatf_from_basis(axes[0], axes[1], axes[2])),
                          .scale = {scale[0], scale[1], scale[2]},
                          .matrix = matrix,
                          .cached = true};
    return true;
}
