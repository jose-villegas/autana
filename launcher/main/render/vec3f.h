/*
 * vec3f: the float 3-vector every float camera in render/ shares.
 *
 * SINGLE PRECISION ONLY. The FPU this runs on has no double, so one stray
 * promotion costs an order of magnitude; a .c including this carries
 * `#pragma GCC diagnostic error "-Wdouble-promotion"` itself, since a pragma
 * in a header would bind every includer.
 */
#pragma once

#include <math.h>

typedef struct {
    float x, y, z;
} vec3f_t;

static inline vec3f_t
vec3f_add(vec3f_t a, vec3f_t b) {
    return (vec3f_t){a.x + b.x, a.y + b.y, a.z + b.z};
}

static inline vec3f_t
vec3f_sub(vec3f_t a, vec3f_t b) {
    return (vec3f_t){a.x - b.x, a.y - b.y, a.z - b.z};
}

static inline vec3f_t
vec3f_scale(vec3f_t a, float s) {
    return (vec3f_t){a.x * s, a.y * s, a.z * s};
}

static inline float
vec3f_dot(vec3f_t a, vec3f_t b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

static inline vec3f_t
vec3f_cross(vec3f_t a, vec3f_t b) {
    return (vec3f_t){a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

static inline vec3f_t
vec3f_normalize(vec3f_t a) {
    return vec3f_scale(a, 1.0f / sqrtf(vec3f_dot(a, a)));
}
