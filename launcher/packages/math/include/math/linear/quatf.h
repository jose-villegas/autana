/*
 * quatf: a float rotation with host-exact, halved-arc polynomial slerp.
 * The rest is quat_template.h's; docs/math/README.md says how it is made.
 */
#pragma once

#include <float.h>
#include <stddef.h>

#include "math/linear/math_const.h"
#include "math/linear/quat_template.h"
#include "math/linear/vec3f.h"
#include "math/scalar/mathf.h"

MATH_DEFINE_QUAT(quatf, vec3f, float, mathf)
MATH_DEFINE_QUAT_NORMALIZE(quatf, vec3f, float, mathf)

/* Shortest-arc unit quaternion interpolation for t in 0..1. b is negated when
 * a . b < 0, the shorter way round, so q and -q give the same path.
 * Eberly's series in cos - 1 halves the arc past 120 degrees and sums until a
 * term is below float precision (docs/Citations.md#68). */
static inline quatf_t
quatf_slerp(quatf_t a, quatf_t b, float t) {
    static const float u[] = {1.0F / (1 * 3),  1.0F / (2 * 5),   1.0F / (3 * 7),   1.0F / (4 * 9),
                              1.0F / (5 * 11), 1.0F / (6 * 13),  1.0F / (7 * 15),  1.0F / (8 * 17),
                              1.0F / (9 * 19), 1.0F / (10 * 21), 1.0F / (11 * 23), 1.0F / (12 * 25)};
    static const float v[] = {1.0F / 3,  2.0F / 5,  3.0F / 7,  4.0F / 9,   5.0F / 11,  6.0F / 13,
                              7.0F / 15, 8.0F / 17, 9.0F / 19, 10.0F / 21, 11.0F / 23, 12.0F / 25};
    float cosine = (a.x * b.x) + (a.y * b.y) + (a.z * b.z) + (a.w * b.w);
    if (cosine < 0.0F) {
        b = (quatf_t){-b.x, -b.y, -b.z, -b.w};
        cosine = -cosine;
    }
    const float halve_past_120_degrees = 0.5F;
    if (cosine < halve_past_120_degrees) {
        const quatf_t midpoint = quatf_normalize((quatf_t){a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w});
        if (t < 0.5F) {
            b = midpoint;
            t = 2.0F * t;
        } else {
            a = midpoint;
            t = 2.0F * t - 1.0F;
        }
        cosine = (a.x * b.x) + (a.y * b.y) + (a.z * b.z) + (a.w * b.w);
    }
    const float cosine_offset = cosine - 1.0F;
    const float complement = 1.0F - t;
    const float t_squared = t * t, complement_squared = complement * complement;
    const float below_float_precision = FLT_EPSILON * 0.25F;
    float ta = 1.0F, tb = 1.0F, wa = 1.0F, wb = 1.0F;
    for (size_t k = 0; k < sizeof u / sizeof u[0]; k++) {
        ta *= (u[k] * complement_squared - v[k]) * cosine_offset;
        tb *= (u[k] * t_squared - v[k]) * cosine_offset;
        wa += ta;
        wb += tb;
        if (fabsf(ta) + fabsf(tb) < below_float_precision) {
            break;
        }
    }
    wa *= complement;
    wb *= t;
    return quatf_normalize(
        (quatf_t){(a.x * wa) + (b.x * wb), (a.y * wa) + (b.y * wb), (a.z * wa) + (b.z * wb), (a.w * wa) + (b.w * wb)});
}
