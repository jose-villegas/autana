/*
 * quatf: a float rotation with host-exact, halved-arc polynomial slerp.
 * The rest is quat_template.h's; docs/math/README.md says how it is made.
 */
#pragma once

#include <float.h>
#include <stddef.h>

#include "math/linear/quat_template.h"
#include "math/linear/vec3f.h"
#include "math/scalar/math_const.h"
#include "math/scalar/mathf.h"

MATH_DEFINE_QUAT(quatf, vec3f, float, mathf)
MATH_DEFINE_QUAT_NORMALIZE(quatf, vec3f, float, mathf)

/* Shortest-arc interpolation, t in 0..1; b is negated when a . b < 0, so q and
 * -q give one path. Nearly parallel inputs take nlerp; the rest Eberly's series
 * in cos - 1 (Eberly 2011), on half the arc past a 120-degree
 * rotation, summed to float precision. */
static inline quatf_t
quatf_slerp(quatf_t a, quatf_t b, float t) {
    /* u[k - 1] = 1 / (k (2k + 1)): term k scales the one before by (s^2 - k^2) u[k - 1] (cos - 1).
     * Twelve terms reach float precision at the halving cosine, the slowest case. */
    static const float u[] = {1.0F / 3,   1.0F / 10,  1.0F / 21,  1.0F / 36,  1.0F / 55,  1.0F / 78,
                              1.0F / 105, 1.0F / 136, 1.0F / 171, 1.0F / 210, 1.0F / 253, 1.0F / 300};
    float cosine = (a.x * b.x) + (a.y * b.y) + (a.z * b.z) + (a.w * b.w);
    /* Tested before the sign flip and marked likely, the dense-key case keeps the series' live values off its path. */
    const float nlerp_from_cosine = 0.9995F; /* nlerp within 4.3 float epsilons of slerp */
    if (__builtin_expect(fabsf(cosine) >= nlerp_from_cosine, 1)) {
        return quatf_nlerp(a, b, t);
    }
    if (cosine < 0.0F) {
        b = (quatf_t){-b.x, -b.y, -b.z, -b.w};
        cosine = -cosine;
    }
    const float halve_below_cosine = 0.5F; /* a 120-degree rotation */
    if (cosine < halve_below_cosine) {
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
        const float term_squared = (float)((k + 1) * (k + 1));
        ta *= (complement_squared - term_squared) * u[k] * cosine_offset;
        tb *= (t_squared - term_squared) * u[k] * cosine_offset;
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
