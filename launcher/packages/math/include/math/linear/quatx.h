/*
 * quatx: a Q16.16 rotation; angles are turns (mathx.h). It has no slerp: that
 * needs an arccosine the fixed-point type does not carry. The operations are
 * quat_template.h's; docs/math/Linear.md says how they are made.
 */
#pragma once

#include "math/linear/quat_template.h"
#include "math/linear/vec3x.h"
#include "math/scalar/mathx.h"

MATH_DEFINE_QUAT(quatx, vec3x, int32_t, mathx)
/* The template squares in Q16.16: suitable for nlerp's near-unit blend.
 * Public normalization below also handles arbitrary magnitudes. */
typedef quatx_t quatx_unit_t;
MATH_DEFINE_QUAT_NORMALIZE(quatx_unit, vec3x, int32_t, mathx)

/* Any nonzero int32 quaternion: component error <= 0.5 + 2^-12 LSB. Zero gives zero. */
static inline quatx_t
quatx_normalize(quatx_t q) {
    const int32_t in[] = {q.x, q.y, q.z, q.w};
    int32_t out[4];
    int32_t quotient;
    mathx_normalize_n(in, out, 4, 0, &quotient);
    return (quatx_t){out[0], out[1], out[2], out[3]};
}

static inline quatx_t
quatx_nlerp(quatx_t a, quatx_t b, int32_t t) {
    return quatx_unit_nlerp(a, b, t);
}

static inline quatx_t
quatx_from_basis(vec3x_t r, vec3x_t u, vec3x_t f) {
    return quatx_unit_from_basis(r, u, f);
}
