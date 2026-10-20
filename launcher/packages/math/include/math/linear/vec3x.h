/*
 * vec3x: a 3-component vector of a Q16.16. The operations are
 * vec3_template.h's and vec_swizzle_template.h's; docs/math/Linear.md says how
 * they are made.
 */
#pragma once

#include <stdint.h>

#include "math/linear/vec2x.h"
#include "math/linear/vec3_template.h"
#include "math/scalar/mathx.h"

MATH_DEFINE_VEC3(vec3x, int32_t, int32_t, mathx)
MATH_DEFINE_VEC_LERP(vec3x, int32_t, mathx, 3)
MATH_DEFINE_VEC_SWIZZLE(vec3x, vec2x, int32_t)

/* out = a/|a|, *q = t/|a|; *q saturates by sign outside Q16.16.
 * A zero vector gives zero and mathx_div(t, 0). */
static inline vec3x_t
vec3x_normalize_divide(vec3x_t a, int32_t t, int32_t* q) {
    enum {
        VEC3X_NORMALIZE_COMPONENT_SHIFT = 29,
        VEC3X_NORMALIZE_TOP_BIT = 31,
        VEC3X_NORMALIZE_RECIPROCAL_SHIFT = VEC3X_NORMALIZE_COMPONENT_SHIFT + VEC3X_NORMALIZE_TOP_BIT,
        VEC3X_NORMALIZE_OUTPUT_SHIFT = VEC3X_NORMALIZE_RECIPROCAL_SHIFT - MATHX_SHIFT,
    };

    _Static_assert((UINT64_C(1) << (2 * (VEC3X_NORMALIZE_COMPONENT_SHIFT + 1))) <= UINT64_MAX / 3,
                   "Three scaled component squares fit uint64");
    _Static_assert((UINT64_C(1) << (VEC3X_NORMALIZE_COMPONENT_SHIFT + 1)) * INT32_MAX <= (UINT64_C(1) << 62),
                   "Scaled component times reciprocal fits with rounding headroom");
    _Static_assert((UINT64_C(1) << VEC3X_NORMALIZE_TOP_BIT) * INT32_MAX <= (UINT64_C(1) << 62),
                   "Numerator times reciprocal fits with rounding headroom");
    const int32_t components[] = {a.x, a.y, a.z};
    uint32_t m = 0;
    for (size_t i = 0; i < sizeof(components) / sizeof(components[0]); i++) {
        const uint32_t magnitude = components[i] < 0 ? (uint32_t)-(int64_t)components[i] : (uint32_t)components[i];
        if (magnitude > m) {
            m = magnitude;
        }
    }
    if (m == 0) {
        *q = mathx_div(t, 0);
        return a;
    }
    const int s = VEC3X_NORMALIZE_COMPONENT_SHIFT - (VEC3X_NORMALIZE_TOP_BIT - __builtin_clz(m));
    int64_t u[3];
    uint64_t sum = 0;
    for (size_t i = 0; i < sizeof(components) / sizeof(components[0]); i++) {
        u[i] = s >= 0 ? (int64_t)components[i] * ((int64_t)1 << s) : fx_round_shift(components[i], -s);
        sum += (uint64_t)(u[i] * u[i]);
    }
    uint64_t len = mathi_isqrt64(sum);
    len += sum - len * len > len;
    const uint64_t reciprocal = ((UINT64_C(1) << VEC3X_NORMALIZE_RECIPROCAL_SHIFT) + len / 2) / len;
    const int32_t k = mathx_saturate((int64_t)reciprocal);
    *q = mathx_saturate(fx_round_shift((int64_t)t * k, VEC3X_NORMALIZE_OUTPUT_SHIFT - s));
    return (vec3x_t){(int32_t)fx_round_shift(u[0] * k, VEC3X_NORMALIZE_OUTPUT_SHIFT),
                     (int32_t)fx_round_shift(u[1] * k, VEC3X_NORMALIZE_OUTPUT_SHIFT),
                     (int32_t)fx_round_shift(u[2] * k, VEC3X_NORMALIZE_OUTPUT_SHIFT)};
}

/* Unit vector; a zero vector gives zero. */
static inline vec3x_t
vec3x_normalize(vec3x_t a) {
    int32_t quotient;
    return vec3x_normalize_divide(a, 0, &quotient);
}
