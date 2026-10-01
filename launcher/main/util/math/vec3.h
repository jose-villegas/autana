/*
 * vec3: three floats for a point, a direction or a scale.
 *
 * Single precision only: the FPU this runs on has no double, so one stray
 * promotion costs an order of magnitude. A .c including this carries
 * `#pragma GCC diagnostic error "-Wdouble-promotion"` itself, since a pragma
 * in a header would bind every includer. Header-only, static inline and
 * ESP-IDF-free.
 */
#pragma once

#include <math.h>

typedef struct {
    float x, y, z;
} vec3_t;

static inline vec3_t
vec3_add(vec3_t a, vec3_t b) {
    return (vec3_t){a.x + b.x, a.y + b.y, a.z + b.z};
}

static inline vec3_t
vec3_sub(vec3_t a, vec3_t b) {
    return (vec3_t){a.x - b.x, a.y - b.y, a.z - b.z};
}

static inline vec3_t
vec3_scale(vec3_t a, float s) {
    return (vec3_t){a.x * s, a.y * s, a.z * s};
}

static inline float
vec3_dot(vec3_t a, vec3_t b) {
    return (a.x * b.x) + (a.y * b.y) + (a.z * b.z);
}

static inline vec3_t
vec3_cross(vec3_t a, vec3_t b) {
    return (vec3_t){(a.y * b.z) - (a.z * b.y), (a.z * b.x) - (a.x * b.z), (a.x * b.y) - (a.y * b.x)};
}

static inline vec3_t
vec3_normalize(vec3_t a) {
    return vec3_scale(a, 1.0F / sqrtf(vec3_dot(a, a)));
}
