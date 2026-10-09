/*
 * quat_template: a rotation as x, y, z, w, unit length, Hamilton product,
 * acting on a vector as q * v * q^-1, for one number type; see
 * math_template.h for the macro arguments. The frame is left-handed, with
 * x right, y up and z forward: a positive angle about +y turns +z toward
 * +x. Angles are radians for float and turns for fixed point. normalize and from_basis need a square root and a
 * divide, so they are a macro of their own.
 */
#pragma once

#include <stdbool.h>

#define MATH_DEFINE_QUAT(P, V, T, OPS)                                                                                 \
    typedef struct {                                                                                                   \
        T x, y, z, w;                                                                                                  \
    } P##_t;                                                                                                           \
                                                                                                                       \
    /* identity = (0, 0, 0, 1), the rotation that does nothing. */                                                     \
    static inline P##_t P##_identity(void) { return (P##_t){OPS##_zero(), OPS##_zero(), OPS##_zero(), OPS##_one()}; }  \
                                                                                                                       \
    /* q = (axis * sin(angle/2), cos(angle/2)); `axis` must be unit length. */                                         \
    static inline P##_t P##_from_axis_angle(V##_t axis, T angle) {                                                     \
        const T s = OPS##_half_sin(angle);                                                                             \
        return (P##_t){OPS##_mul(axis.x, s), OPS##_mul(axis.y, s), OPS##_mul(axis.z, s), OPS##_half_cos(angle)};       \
    }                                                                                                                  \
                                                                                                                       \
    /* a * b, b applied first: */                                                                                      \
    /*   x = aw*bx + ax*bw + ay*bz - az*by */                                                                          \
    /*   y = aw*by - ax*bz + ay*bw + az*bx */                                                                          \
    /*   z = aw*bz + ax*by - ay*bx + az*bw */                                                                          \
    /*   w = aw*bw - ax*bx - ay*by - az*bz */                                                                          \
    static inline P##_t P##_mul(P##_t a, P##_t b) {                                                                    \
        return (P##_t){                                                                                                \
            OPS##_sub(OPS##_add(OPS##_add(OPS##_mul(a.w, b.x), OPS##_mul(a.x, b.w)), OPS##_mul(a.y, b.z)),             \
                      OPS##_mul(a.z, b.y)),                                                                            \
            OPS##_add(OPS##_add(OPS##_sub(OPS##_mul(a.w, b.y), OPS##_mul(a.x, b.z)), OPS##_mul(a.y, b.w)),             \
                      OPS##_mul(a.z, b.x)),                                                                            \
            OPS##_add(OPS##_sub(OPS##_add(OPS##_mul(a.w, b.z), OPS##_mul(a.x, b.y)), OPS##_mul(a.y, b.x)),             \
                      OPS##_mul(a.z, b.w)),                                                                            \
            OPS##_sub(OPS##_sub(OPS##_sub(OPS##_mul(a.w, b.w), OPS##_mul(a.x, b.x)), OPS##_mul(a.y, b.y)),             \
                      OPS##_mul(a.z, b.z)),                                                                            \
        };                                                                                                             \
    }                                                                                                                  \
                                                                                                                       \
    /* q = qy * qx * qz, each from_axis_angle about its axis: Z, then X, then Y */                                     \
    /* about the fixed axes. */                                                                                        \
    static inline P##_t P##_from_euler(V##_t angles) {                                                                 \
        const P##_t about_x = P##_from_axis_angle((V##_t){OPS##_one(), OPS##_zero(), OPS##_zero()}, angles.x);         \
        const P##_t about_y = P##_from_axis_angle((V##_t){OPS##_zero(), OPS##_one(), OPS##_zero()}, angles.y);         \
        const P##_t about_z = P##_from_axis_angle((V##_t){OPS##_zero(), OPS##_zero(), OPS##_one()}, angles.z);         \
        return P##_mul(about_y, P##_mul(about_x, about_z));                                                            \
    }                                                                                                                  \
                                                                                                                       \
    /* With u = (qx, qy, qz) and t = 2 (u x v): v' = v + qw*t + u x t, which is q v q^-1 */                            \
    /* expanded for a unit `q`. */                                                                                     \
    static inline V##_t P##_rotate(P##_t q, V##_t v) {                                                                 \
        const V##_t u = {q.x, q.y, q.z};                                                                               \
        const V##_t t = V##_scale(V##_cross(u, v), OPS##_two());                                                       \
        return V##_add(V##_add(v, V##_scale(t, q.w)), V##_cross(u, t));                                                \
    }

#define MATH_DEFINE_QUAT_NORMALIZE(P, V, T, OPS)                                                                       \
    /* normalize = q * (1 / sqrt(qx*qx + qy*qy + qz*qz + qw*qw)). Precondition: `q` is not zero. */                    \
    static inline P##_t P##_normalize(P##_t q) {                                                                       \
        const T k = OPS##_div(                                                                                         \
            OPS##_one(),                                                                                               \
            OPS##_sqrt(OPS##_add(OPS##_add(OPS##_add(OPS##_mul(q.x, q.x), OPS##_mul(q.y, q.y)), OPS##_mul(q.z, q.z)),  \
                                 OPS##_mul(q.w, q.w))));                                                               \
        return (P##_t){OPS##_mul(q.x, k), OPS##_mul(q.y, k), OPS##_mul(q.z, k), OPS##_mul(q.w, k)};                    \
    }                                                                                                                  \
                                                                                                                       \
    /* Axes r, u, f as columns. If rx + uy + fz > 0: s = 2 sqrt(1 + rx + uy + fz), */                                  \
    /*   q = ((uz - fy)/s, (fx - rz)/s, (ry - ux)/s, s/4). */                                                          \
    /* Else s is from the largest diagonal, e.g. s = 2 sqrt(1 + rx - uy - fz), */                                      \
    /*   q = (s/4, (ux + ry)/s, (fx + rz)/s, (uz - fy)/s): a small s loses precision. */                               \
    static inline P##_t P##_from_basis(V##_t r, V##_t u, V##_t f) {                                                    \
        const T trace = OPS##_add(OPS##_add(r.x, u.y), f.z);                                                           \
        T s;                                                                                                           \
        if (trace > OPS##_zero()) {                                                                                    \
            s = OPS##_mul(OPS##_sqrt(OPS##_add(trace, OPS##_one())), OPS##_two());                                     \
            return (P##_t){OPS##_div(OPS##_sub(u.z, f.y), s), OPS##_div(OPS##_sub(f.x, r.z), s),                       \
                           OPS##_div(OPS##_sub(r.y, u.x), s), OPS##_div(s, OPS##_four())};                             \
        }                                                                                                              \
        if (r.x > u.y && r.x > f.z) {                                                                                  \
            s = OPS##_mul(OPS##_sqrt(OPS##_sub(OPS##_sub(OPS##_add(OPS##_one(), r.x), u.y), f.z)), OPS##_two());       \
            return (P##_t){OPS##_div(s, OPS##_four()), OPS##_div(OPS##_add(u.x, r.y), s),                              \
                           OPS##_div(OPS##_add(f.x, r.z), s), OPS##_div(OPS##_sub(u.z, f.y), s)};                      \
        }                                                                                                              \
        if (u.y > f.z) {                                                                                               \
            s = OPS##_mul(OPS##_sqrt(OPS##_sub(OPS##_sub(OPS##_add(OPS##_one(), u.y), r.x), f.z)), OPS##_two());       \
            return (P##_t){OPS##_div(OPS##_add(u.x, r.y), s), OPS##_div(s, OPS##_four()),                              \
                           OPS##_div(OPS##_add(f.y, u.z), s), OPS##_div(OPS##_sub(f.x, r.z), s)};                      \
        }                                                                                                              \
        s = OPS##_mul(OPS##_sqrt(OPS##_sub(OPS##_sub(OPS##_add(OPS##_one(), f.z), r.x), u.y)), OPS##_two());           \
        return (P##_t){OPS##_div(OPS##_add(f.x, r.z), s), OPS##_div(OPS##_add(f.y, u.z), s),                           \
                       OPS##_div(s, OPS##_four()), OPS##_div(OPS##_sub(r.y, u.x), s)};                                 \
    }
