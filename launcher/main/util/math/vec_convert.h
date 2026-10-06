/*
 * vec_convert: moving a vector between number types, always by an explicit
 * call named for its destination and source (vec3f_from_vec3x,
 * vec3s_from_vec3f, ...), never an implicit cast.
 *
 *   float <-> int32   exact one way, rounded to nearest and saturated back
 *   float <-> int16   through a `scale`: one int16 step is `scale` units, so
 *                     the int16 range is +-32767 * scale; a value outside it
 *                     saturates
 *   float <-> Q16.16  scaled by 65536; a float outside +-32768 saturates
 *   int32 <-> int16   widens exactly; narrowing saturates
 *
 * A round trip returns the original exactly where the destination can hold
 * it (int <-> float, int16 <-> int32 inside its range, Q16.16 values from a
 * float multiple of 1/65536), and the nearest representable value otherwise.
 * Needs libm for lroundf. Header-only and static inline.
 */
#pragma once

#include <math.h>
#include <stdint.h>

#include "util/math/vec2f.h"
#include "util/math/vec2i.h"
#include "util/math/vec2s.h"
#include "util/math/vec2x.h"
#include "util/math/vec3f.h"
#include "util/math/vec3i.h"
#include "util/math/vec3s.h"
#include "util/math/vec3x.h"
#include "util/scalar/maths.h"
#include "util/scalar/mathx.h"

/* The largest float below 2^31, so a clamped value still converts. */
#define MATH_FLOAT_INT_LIMIT 2147483520.0F

/* Round to the nearest int32, ties away from zero, saturating at the endpoints. */
static inline int32_t
mathf_round_i32(float v) {
    if (v >= MATH_FLOAT_INT_LIMIT) {
        return INT32_MAX;
    }
    return v <= -MATH_FLOAT_INT_LIMIT ? INT32_MIN : (int32_t)lroundf(v);
}

/* Round scaled float units to a saturated int16 coordinate. */
static inline int16_t
mathf_round_s(float v, float scale) {
    return maths_saturate(mathf_round_i32(v / scale));
}

/* Convert float units to rounded and saturated Q16.16. */
static inline int32_t
mathf_to_x(float v) {
    return mathf_round_i32(v * (float)MATHX_ONE);
}

/* Convert Q16.16 units to single-precision float. */
static inline float
mathx_to_f(int32_t v) {
    return (float)v * (1.0F / (float)MATHX_ONE);
}

/* Convert a 2-component vector using single-precision casts. */
static inline vec2f_t
vec2f_from_vec2i(vec2i_t v) {
    return (vec2f_t){(float)v.x, (float)v.y};
}

/* Convert a 2-component vector rounding to nearest with ties away from zero and saturation. */
static inline vec2i_t
vec2i_from_vec2f(vec2f_t v) {
    return (vec2i_t){mathf_round_i32(v.x), mathf_round_i32(v.y)};
}

/* Convert a 2-component vector multiplying by the caller scale. */
static inline vec2f_t
vec2f_from_vec2s(vec2s_t v, float scale) {
    return (vec2f_t){(float)v.x * scale, (float)v.y * scale};
}

/* Convert a 2-component vector dividing by the caller scale, rounding and saturating. */
static inline vec2s_t
vec2s_from_vec2f(vec2f_t v, float scale) {
    return (vec2s_t){mathf_round_s(v.x, scale), mathf_round_s(v.y, scale)};
}

/* Convert a 2-component vector converting Q16.16 to float units. */
static inline vec2f_t
vec2f_from_vec2x(vec2x_t v) {
    return (vec2f_t){mathx_to_f(v.x), mathx_to_f(v.y)};
}

/* Convert a 2-component vector rounding and saturating float units to Q16.16. */
static inline vec2x_t
vec2x_from_vec2f(vec2f_t v) {
    return (vec2x_t){mathf_to_x(v.x), mathf_to_x(v.y)};
}

/* Convert a 2-component vector widening components exactly. */
static inline vec2i_t
vec2i_from_vec2s(vec2s_t v) {
    return (vec2i_t){v.x, v.y};
}

/* Convert a 2-component vector saturating components to int16. */
static inline vec2s_t
vec2s_from_vec2i(vec2i_t v) {
    return (vec2s_t){maths_saturate(v.x), maths_saturate(v.y)};
}

/* Convert a 3-component vector using single-precision casts. */
static inline vec3f_t
vec3f_from_vec3i(vec3i_t v) {
    return (vec3f_t){(float)v.x, (float)v.y, (float)v.z};
}

/* Convert a 3-component vector rounding to nearest with ties away from zero and saturation. */
static inline vec3i_t
vec3i_from_vec3f(vec3f_t v) {
    return (vec3i_t){mathf_round_i32(v.x), mathf_round_i32(v.y), mathf_round_i32(v.z)};
}

/* Convert a 3-component vector multiplying by the caller scale. */
static inline vec3f_t
vec3f_from_vec3s(vec3s_t v, float scale) {
    return (vec3f_t){(float)v.x * scale, (float)v.y * scale, (float)v.z * scale};
}

/* Convert a 3-component vector dividing by the caller scale, rounding and saturating. */
static inline vec3s_t
vec3s_from_vec3f(vec3f_t v, float scale) {
    return (vec3s_t){mathf_round_s(v.x, scale), mathf_round_s(v.y, scale), mathf_round_s(v.z, scale)};
}

/* Convert a 3-component vector converting Q16.16 to float units. */
static inline vec3f_t
vec3f_from_vec3x(vec3x_t v) {
    return (vec3f_t){mathx_to_f(v.x), mathx_to_f(v.y), mathx_to_f(v.z)};
}

/* Convert a 3-component vector rounding and saturating float units to Q16.16. */
static inline vec3x_t
vec3x_from_vec3f(vec3f_t v) {
    return (vec3x_t){mathf_to_x(v.x), mathf_to_x(v.y), mathf_to_x(v.z)};
}

/* Convert a 3-component vector widening components exactly. */
static inline vec3i_t
vec3i_from_vec3s(vec3s_t v) {
    return (vec3i_t){v.x, v.y, v.z};
}

/* Convert a 3-component vector saturating components to int16. */
static inline vec3s_t
vec3s_from_vec3i(vec3i_t v) {
    return (vec3s_t){maths_saturate(v.x), maths_saturate(v.y), maths_saturate(v.z)};
}
