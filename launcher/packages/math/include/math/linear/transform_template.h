/*
 * transform_template: where a thing is, as a position, a rotation and a
 * scale, for one number type; see docs/math/Conventions.md#templates for the macro arguments. Position,
 * rotation and scale are the truth; the model matrix is derived and cached.
 *
 * Which matrix call to use:
 *   transform_matrix(t)          the model matrix, built once and kept in `t`
 *                                until a setter, translate, rotate or look_at
 *                                changes it: for an object a frame reads
 *                                again and again;
 *   transform_compute_matrix(t)  a fresh matrix from a const transform that
 *                                is never written, for read-only code.
 * Code changes a transform only through the setters, which clear `cached`;
 * a zero-initialized transform has `cached` false and so builds on first use.
 *
 * Local +x is right, +y up and +z forward, so a camera looks down +z.
 */
#pragma once

#include <stdbool.h>

#define MATH_DEFINE_TRANSFORM(P, V, Q, M, OPS)                                                                         \
                                                                                                                       \
    typedef struct {                                                                                                   \
        V##_t position;                                                                                                \
        Q##_t rotation;                                                                                                \
        V##_t scale;                                                                                                   \
        M##_t matrix; /* valid when `cached` */                                                                        \
        bool cached;                                                                                                   \
    } P##_t;                                                                                                           \
                                                                                                                       \
    /* position = p, cached = false; likewise rotation and scale. */                                                   \
    static inline void P##_set_position(P##_t* t, V##_t position) {                                                    \
        t->position = position;                                                                                        \
        t->cached = false;                                                                                             \
    }                                                                                                                  \
                                                                                                                       \
    static inline void P##_set_rotation(P##_t* t, Q##_t rotation) {                                                    \
        t->rotation = rotation;                                                                                        \
        t->cached = false;                                                                                             \
    }                                                                                                                  \
                                                                                                                       \
    static inline void P##_set_scale(P##_t* t, V##_t scale) {                                                          \
        t->scale = scale;                                                                                              \
        t->cached = false;                                                                                             \
    }                                                                                                                  \
                                                                                                                       \
    /* position = position + delta, in the parent's frame. */                                                          \
    static inline void P##_translate(P##_t* t, V##_t delta) { P##_set_position(t, V##_add(t->position, delta)); }      \
                                                                                                                       \
    /* rotation = normalize(rotation * delta): about the transform's own axes. */                                      \
    static inline void P##_rotate(P##_t* t, Q##_t delta) {                                                             \
        P##_set_rotation(t, Q##_normalize(Q##_mul(t->rotation, delta)));                                               \
    }                                                                                                                  \
                                                                                                                       \
    /* M = T(position) * R(rotation) * S(scale), model to parent. */                                                   \
    static inline M##_t P##_compute_matrix(const P##_t* t) {                                                           \
        return M##_from_trs(t->position, t->rotation, t->scale);                                                       \
    }                                                                                                                  \
                                                                                                                       \
    /* M as compute_matrix, rebuilt only while `cached` is false. */                                                   \
    static inline M##_t P##_matrix(P##_t* t) {                                                                         \
        if (!t->cached) {                                                                                              \
            t->matrix = P##_compute_matrix(t);                                                                         \
            t->cached = true;                                                                                          \
        }                                                                                                              \
        return t->matrix;                                                                                              \
    }                                                                                                                  \
                                                                                                                       \
    /* V = R^T * T(-position): rotation part R^T, column 3 = -(R^T * position). The inverse */                         \
    /* of position and rotation, parent to local; scale is ignored. */                                                 \
    static inline M##_t P##_view(const P##_t* t) {                                                                     \
        const V##_t origin = {OPS##_zero(), OPS##_zero(), OPS##_zero()};                                               \
        const V##_t one = {OPS##_one(), OPS##_one(), OPS##_one()};                                                     \
        const M##_t pose = M##_from_trs(origin, t->rotation, one);                                                     \
        const V##_t x = {pose.m[0][0], pose.m[1][0], pose.m[2][0]}, y = {pose.m[0][1], pose.m[1][1], pose.m[2][1]},    \
                    z = {pose.m[0][2], pose.m[1][2], pose.m[2][2]};                                                    \
        return M##_view_from_axes(x, y, z, t->position);                                                               \
    }                                                                                                                  \
                                                                                                                       \
    /* f = normalize(direction), r = normalize(up x f), u = f x r, */                                                  \
    /* rotation = from_basis(r, u, f). `up` must not be parallel to `direction`, */                                    \
    /* and `direction` must be nonzero. */                                                                             \
    static inline void P##_look_along(P##_t* t, V##_t direction, V##_t up) {                                           \
        const V##_t forward = V##_normalize(direction);                                                                \
        const V##_t right = V##_normalize(V##_cross(up, forward));                                                     \
        const V##_t above = V##_cross(forward, right);                                                                 \
        P##_set_rotation(t, Q##_from_basis(right, above, forward));                                                    \
    }                                                                                                                  \
                                                                                                                       \
    /* Faces target - position, with the same constraints as look_along. */                                            \
    static inline void P##_look_at(P##_t* t, V##_t target, V##_t up) {                                                 \
        P##_look_along(t, V##_sub(target, t->position), up);                                                           \
    }                                                                                                                  \
                                                                                                                       \
    /* Unit scale at position, facing forward; forward must be nonzero and not parallel to up. */                      \
    static inline P##_t P##_looking(V##_t position, V##_t forward, V##_t up) {                                         \
        P##_t pose = {0};                                                                                              \
        pose.position = position;                                                                                      \
        pose.scale = (V##_t){OPS##_one(), OPS##_one(), OPS##_one()};                                                   \
        P##_look_along(&pose, forward, up);                                                                            \
        return pose;                                                                                                   \
    }
