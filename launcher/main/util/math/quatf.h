/*
 * quatf: a float rotation, plus the one operation only floats have, slerp.
 * The rest is quat_template.h's; math_template.h says how it is made.
 */
#pragma once

#include "util/math/math_const.h"
#include "util/math/mathf.h"
#include "util/math/quat_template.h"
#include "util/math/vec3f.h"

MATH_DEFINE_QUAT(quatf, vec3f, float, mathf)
MATH_DEFINE_QUAT_NORMALIZE(quatf, vec3f, float, mathf)

/* slerp = a sin((1 - t) O) / sin O + b sin(t O) / sin O, with cos O = a . b.
 * b is negated when a . b < 0, the shorter way round, so q and -q give the
 * same path. Nearly parallel inputs fall back to a normalized lerp, where the
 * sine in the divisor vanishes. */
static inline quatf_t
quatf_slerp(quatf_t a, quatf_t b, float t) {
    float cosine = (a.x * b.x) + (a.y * b.y) + (a.z * b.z) + (a.w * b.w);
    if (cosine < 0.0F) {
        b = (quatf_t){-b.x, -b.y, -b.z, -b.w};
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
    return quatf_normalize(
        (quatf_t){(a.x * wa) + (b.x * wb), (a.y * wa) + (b.y * wb), (a.z * wa) + (b.z * wb), (a.w * wa) + (b.w * wb)});
}
