/*
 * quat: a rotation as x, y, z, w, unit length, Hamilton product, acting on
 * a vector as q * v * q^-1. A positive angle about an axis is the right-hand
 * rule's matrix; in the renderer's frame (x right, y up, z forward) that
 * turns +z toward +x about +y. Float only, see vec3.h for the double rule.
 */
#pragma once

#include <math.h>

#include "util/math/vec3.h"

#define MATH_PI  3.14159265359F
#define MATH_TAU 6.28318530718F

typedef struct {
    float x, y, z, w;
} quat_t;

static inline quat_t
quat_identity(void) {
    return (quat_t){0.0F, 0.0F, 0.0F, 1.0F};
}

/* `axis` must be unit length. */
static inline quat_t
quat_from_axis_angle(vec3_t axis, float radians) {
    const float half = radians * 0.5F;
    const float s = sinf(half);
    return (quat_t){axis.x * s, axis.y * s, axis.z * s, cosf(half)};
}

/* a * b: b is applied first, then a. */
static inline quat_t
quat_mul(quat_t a, quat_t b) {
    return (quat_t){
        (a.w * b.x) + (a.x * b.w) + (a.y * b.z) - (a.z * b.y),
        (a.w * b.y) - (a.x * b.z) + (a.y * b.w) + (a.z * b.x),
        (a.w * b.z) + (a.x * b.y) - (a.y * b.x) + (a.z * b.w),
        (a.w * b.w) - (a.x * b.x) - (a.y * b.y) - (a.z * b.z),
    };
}

/* Euler angles in radians, applied Z, then X, then Y about the fixed axes,
 * the order Unity uses. */
static inline quat_t
quat_from_euler(vec3_t radians) {
    const quat_t about_x = quat_from_axis_angle((vec3_t){1.0F, 0.0F, 0.0F}, radians.x);
    const quat_t about_y = quat_from_axis_angle((vec3_t){0.0F, 1.0F, 0.0F}, radians.y);
    const quat_t about_z = quat_from_axis_angle((vec3_t){0.0F, 0.0F, 1.0F}, radians.z);
    return quat_mul(about_y, quat_mul(about_x, about_z));
}

static inline quat_t
quat_normalize(quat_t q) {
    const float k = 1.0F / sqrtf((q.x * q.x) + (q.y * q.y) + (q.z * q.z) + (q.w * q.w));
    return (quat_t){q.x * k, q.y * k, q.z * k, q.w * k};
}

static inline vec3_t
quat_rotate(quat_t q, vec3_t v) {
    const vec3_t u = {q.x, q.y, q.z};
    const vec3_t t = vec3_scale(vec3_cross(u, v), 2.0F);
    return vec3_add(vec3_add(v, vec3_scale(t, q.w)), vec3_cross(u, t));
}

/* The shorter way round, so q and -q give the same path. Nearly parallel
 * inputs fall back to a normalized lerp, where the sine in the divisor
 * vanishes. */
static inline quat_t
quat_slerp(quat_t a, quat_t b, float t) {
    float cosine = (a.x * b.x) + (a.y * b.y) + (a.z * b.z) + (a.w * b.w);
    if (cosine < 0.0F) {
        b = (quat_t){-b.x, -b.y, -b.z, -b.w};
        cosine = -cosine;
    }

    float wa = 1.0F - t;
    float wb = t;
    if (cosine < 0.9995F) {
        const float angle = acosf(cosine);
        const float inv_sine = 1.0F / sinf(angle);
        wa = sinf((1.0F - t) * angle) * inv_sine;
        wb = sinf(t * angle) * inv_sine;
    }
    return quat_normalize(
        (quat_t){(a.x * wa) + (b.x * wb), (a.y * wa) + (b.y * wb), (a.z * wa) + (b.z * wb), (a.w * wa) + (b.w * wb)});
}
