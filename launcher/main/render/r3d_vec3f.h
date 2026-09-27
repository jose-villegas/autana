/*
 * r3d_vec3f - the float 3-vector every float camera and path in render/
 * shares. Single precision only, for the reason r3d_ray.h gives.
 */
#pragma once

#include <math.h>

typedef struct {
    float x, y, z;
} r3d_vec3f_t;

static inline r3d_vec3f_t
r3d_vec3f_add(r3d_vec3f_t a, r3d_vec3f_t b) {
    return (r3d_vec3f_t){a.x + b.x, a.y + b.y, a.z + b.z};
}

static inline r3d_vec3f_t
r3d_vec3f_sub(r3d_vec3f_t a, r3d_vec3f_t b) {
    return (r3d_vec3f_t){a.x - b.x, a.y - b.y, a.z - b.z};
}

static inline r3d_vec3f_t
r3d_vec3f_scale(r3d_vec3f_t a, float s) {
    return (r3d_vec3f_t){a.x * s, a.y * s, a.z * s};
}

static inline float
r3d_vec3f_dot(r3d_vec3f_t a, r3d_vec3f_t b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

static inline r3d_vec3f_t
r3d_vec3f_cross(r3d_vec3f_t a, r3d_vec3f_t b) {
    return (r3d_vec3f_t){a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

static inline r3d_vec3f_t
r3d_vec3f_normalize(r3d_vec3f_t a) {
    return r3d_vec3f_scale(a, 1.0f / sqrtf(r3d_vec3f_dot(a, a)));
}
