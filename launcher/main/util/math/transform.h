/*
 * transform: where a thing is, as Unity's Transform holds it. Position,
 * rotation and scale are the truth; the model matrix is derived from them
 * and cached, rebuilt by transform_matrix() only after a setter, translate,
 * rotate or look_at changed something. Code reads and writes the three
 * parts through the helpers below and never edits the cache.
 *
 * Local +x is right, +y up and +z forward, so a camera looks down +z.
 * Float only, see vec3.h.
 */
#pragma once

#include <stdbool.h>

#include "util/math/mat4.h"
#include "util/math/quat.h"
#include "util/math/vec3.h"

typedef struct {
    vec3_t position;
    quat_t rotation;
    vec3_t scale;
    mat4_t matrix; /* valid when !dirty */
    bool dirty;
} transform_t;

/* An initializer: `transform_t t = TRANSFORM_IDENTITY;`, or
 * `t = (transform_t)TRANSFORM_IDENTITY;`. */
#define TRANSFORM_IDENTITY                                                                                             \
    {.position = {0.0F, 0.0F, 0.0F},                                                                                   \
     .rotation = {0.0F, 0.0F, 0.0F, 1.0F},                                                                             \
     .scale = {1.0F, 1.0F, 1.0F},                                                                                      \
     .matrix =                                                                                                         \
         {{{1.0F, 0.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 1.0F, 0.0F}, {0.0F, 0.0F, 0.0F, 1.0F}}},   \
     .dirty = false}

static inline vec3_t
transform_position(const transform_t* t) {
    return t->position;
}

static inline quat_t
transform_rotation(const transform_t* t) {
    return t->rotation;
}

static inline vec3_t
transform_scale(const transform_t* t) {
    return t->scale;
}

static inline void
transform_set_position(transform_t* t, vec3_t position) {
    t->position = position;
    t->dirty = true;
}

static inline void
transform_set_rotation(transform_t* t, quat_t rotation) {
    t->rotation = rotation;
    t->dirty = true;
}

static inline void
transform_set_scale(transform_t* t, vec3_t scale) {
    t->scale = scale;
    t->dirty = true;
}

/* Moves by `delta` in the parent's frame. */
static inline void
transform_translate(transform_t* t, vec3_t delta) {
    transform_set_position(t, vec3_add(t->position, delta));
}

/* Turns by `delta` about the transform's own axes, as Unity's Rotate does. */
static inline void
transform_rotate(transform_t* t, quat_t delta) {
    transform_set_rotation(t, quat_normalize(quat_mul(t->rotation, delta)));
}

/* Model to parent: scale, then rotate, then translate. */
static inline mat4_t
transform_matrix(transform_t* t) {
    if (t->dirty) {
        t->matrix = mat4_from_trs(t->position, t->rotation, t->scale);
        t->dirty = false;
    }
    return t->matrix;
}

/* Parent to local: the inverse of the position and rotation, for a camera.
 * The scale is ignored, as a camera's is. */
static inline mat4_t
transform_view(const transform_t* t) {
    const mat4_t pose = mat4_from_trs((vec3_t){0.0F, 0.0F, 0.0F}, t->rotation, (vec3_t){1.0F, 1.0F, 1.0F});
    mat4_t view = mat4_identity();
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) {
            view.m[r][c] = pose.m[c][r];
        }
        view.m[r][3] =
            -((view.m[r][0] * t->position.x) + (view.m[r][1] * t->position.y) + (view.m[r][2] * t->position.z));
    }
    return view;
}

/* The rotation whose right, up and forward axes are the columns r, u and f. */
static inline quat_t
transform_axes_to_rotation(vec3_t r, vec3_t u, vec3_t f) {
    const float trace = r.x + u.y + f.z;
    float s;
    if (trace > 0.0F) {
        s = sqrtf(trace + 1.0F) * 2.0F;
        return (quat_t){(u.z - f.y) / s, (f.x - r.z) / s, (r.y - u.x) / s, s / 4.0F};
    }
    if (r.x > u.y && r.x > f.z) {
        s = sqrtf(1.0F + r.x - u.y - f.z) * 2.0F;
        return (quat_t){s / 4.0F, (u.x + r.y) / s, (f.x + r.z) / s, (u.z - f.y) / s};
    }
    if (u.y > f.z) {
        s = sqrtf(1.0F + u.y - r.x - f.z) * 2.0F;
        return (quat_t){(u.x + r.y) / s, s / 4.0F, (f.y + u.z) / s, (f.x - r.z) / s};
    }
    s = sqrtf(1.0F + f.z - r.x - u.y) * 2.0F;
    return (quat_t){(f.x + r.z) / s, (f.y + u.z) / s, s / 4.0F, (r.y - u.x) / s};
}

/* Turns to face `target` with `up` as the sky. Neither may be parallel to
 * the line to the target. */
static inline void
transform_look_at(transform_t* t, vec3_t target, vec3_t up) {
    const vec3_t forward = vec3_normalize(vec3_sub(target, t->position));
    const vec3_t right = vec3_normalize(vec3_cross(up, forward));
    const vec3_t above = vec3_cross(forward, right);
    transform_set_rotation(t, transform_axes_to_rotation(right, above, forward));
}
