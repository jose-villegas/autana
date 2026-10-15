/*
 * quatf: a float rotation with host-exact, halved-arc polynomial slerp.
 * The rest is quat_template.h's; docs/math/README.md says how it is made.
 */
#pragma once

#include "math/linear/math_const.h"
#include "math/linear/quat_template.h"
#include "math/linear/vec3f.h"
#include "math/scalar/mathf.h"

MATH_DEFINE_QUAT(quatf, vec3f, float, mathf)
MATH_DEFINE_QUAT_NORMALIZE(quatf, vec3f, float, mathf)

/* Shortest-arc unit quaternion interpolation for t in 0..1. Halving the arc
 * bounds Eberly's polynomial error below float precision (docs/Citations.md#68).
 * Only arithmetic and sqrtf keep samples host-exact with contraction disabled. */
static inline quatf_t
quatf_slerp(quatf_t a, quatf_t b, float t) {
    const float cosine = (a.x * b.x) + (a.y * b.y) + (a.z * b.z) + (a.w * b.w);
    if (cosine < 0.0F) {
        b = (quatf_t){-b.x, -b.y, -b.z, -b.w};
    }
    const quatf_t midpoint = quatf_normalize((quatf_t){a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w});
    if (t < 0.5F) {
        b = midpoint;
        t = 2.0F * t;
    } else {
        a = midpoint;
        t = 2.0F * t - 1.0F;
    }
    const float cosine_offset = (a.x * b.x) + (a.y * b.y) + (a.z * b.z) + (a.w * b.w) - 1.0F;
    const float complement = 1.0F - t;
    const float t_squared = t * t, complement_squared = complement * complement;
    const int series_terms = 8;
    float wa = 1.0F, wb = 1.0F;
    for (int i = series_terms; i > 0; i--) {
        const float index = (float)i;
        const float denominator = 2.0F * index + 1.0F;
        const float u = 1.0F / (index * denominator), v = index / denominator;
        wa = 1.0F + (u * complement_squared - v) * cosine_offset * wa;
        wb = 1.0F + (u * t_squared - v) * cosine_offset * wb;
    }
    wa *= complement;
    wb *= t;
    return quatf_normalize(
        (quatf_t){(a.x * wa) + (b.x * wb), (a.y * wa) + (b.y * wb), (a.z * wa) + (b.z * wb), (a.w * wa) + (b.w * wb)});
}
