/*
 * mathf: single-precision float's scalar operations for the math templates
 * (docs/math/Scalar.md). The FPU has no double, so one stray promotion costs an
 * order of magnitude; main and every package refuse the promotion in their
 * CMakeLists.txt. Angles are radians. Needs libm for sqrtf,
 * sinf and cosf. A `w` operation is the one a dot product sums with.
 */
#pragma once

#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdint.h>

#ifdef __XTENSA__
#include "sdkconfig.h"
#endif

static inline float
mathf_zero(void) {
    return 0.0F;
}

static inline float
mathf_one(void) {
    return 1.0F;
}

static inline float
mathf_half(void) {
    return 0.5F;
}

static inline float
mathf_abs(float a) {
    return fabsf(a);
}

static inline float
mathf_two(void) {
    return 2.0F;
}

static inline float
mathf_four(void) {
    return 4.0F;
}

static inline float
mathf_clamp(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline float
mathf_add(float a, float b) {
    return a + b;
}

static inline float
mathf_sub(float a, float b) {
    return a - b;
}

static inline float
mathf_neg(float a) {
    return -a;
}

static inline float
mathf_mul(float a, float b) {
    return a * b;
}

static inline float
mathf_div(float a, float b) {
    return a / b;
}

/* Nonzero inputs away from float range limits; CONFIG_MATH_FPU_RECIP enables the seed. */
/* For nonzero input a, reciprocal seed y with two Newton corrections. | y = y + y(1 - ay) | signed magnitudes and power-of-two edges */

/* source: Muller et al. 2018 */
/* exact: y = 1/a */
/* MATH_FAST_PATH(mathf_recip_f, div_recip_f, MATHF_RECIP_ULP_LIMIT) */

static inline float
mathf_recip(float a) {
#if defined(__XTENSA__) && CONFIG_MATH_FPU_RECIP
    float y;
    __asm__("recip0.s %0, %1" : "=f"(y) : "f"(a));
    y += y * (1.0F - (a * y));
    return y + (y * (1.0F - (a * y)));
#else
    return 1.0F / a;
#endif
}

/* Nonzero normal inputs of either sign with normal reciprocals, away from float range limits. */
/* For nonzero input a, reciprocal seed y with one Newton correction. | y = y + y(1 - ay) | all mantissas in [1,2), both signs, normal exponent strides and edges */

/* source: Muller et al. 2018 */
/* exact: y = 1/a */
/* MATH_FAST_PATH(mathf_recip_approx_f, div_recip_f, MATHF_RECIP_APPROX_RELATIVE_LIMIT) */

static inline __attribute__((always_inline)) float
mathf_recip_approx(float a) {
#if defined(__XTENSA__) && CONFIG_MATH_FPU_RECIP
    float y;
    __asm__("recip0.s %0, %1" : "=f"(y) : "f"(a));
    return y + y * (1.0F - a * y);
#else
    return 1.0F / a;
#endif
}

/* The board proof's maximum rounded up to a power of two: the approximation keeps this many bits. */
#define MATHF_RECIP_APPROX_BITS           14
#define MATHF_RECIP_APPROX_RELATIVE_LIMIT (1.0F / (float)(1UL << MATHF_RECIP_APPROX_BITS))
/* One seed correction trades accuracy for throughput; the suite proves this bound. */
#define MATHF_RSQRT_APPROX_RELATIVE_LIMIT 0.0001F
/* Two seed corrections must stay close to the correctly rounded reciprocal root. */
#define MATHF_RSQRT_ULP_LIMIT             1
/* The reciprocal sweep guards the two-correction path against division. */
#define MATHF_RECIP_ULP_LIMIT             1
/* Vector component sweeps bound the error introduced by reciprocal-length scaling. */
#define MATHF_NORMALIZE_ULP_LIMIT         1
/* Quaternion interpolation compounds reciprocal-length scaling and blend rounding. */
#define MATHF_NLERP_ULP_LIMIT             2

/* One correction serves callers that accept the relative error limit for positive normal inputs. */
/* For positive input v, reciprocal-root seed y with one Newton correction. | y = y(3/2 - (v/2)y^2) | all mantissas in [1,4), normal exponent strides and edges */

/* source: Muller et al. 2018 */
/* exact: y = 1/\sqrt{v} */
/* MATH_FAST_PATH(mathf_rsqrt_approx_f, div_rsqrt_f, MATHF_RSQRT_APPROX_RELATIVE_LIMIT) */

static inline __attribute__((always_inline)) float
mathf_rsqrt_approx(float value) {
#if defined(__XTENSA__) && CONFIG_MATH_FPU_RECIP
    float seed;
    __asm__("rsqrt0.s %0, %1" : "=f"(seed) : "f"(value));
    return seed * (1.5F - 0.5F * value * seed * seed);
#else
    return 1.0F / sqrtf(value);
#endif
}

#if defined(__XTENSA__) && CONFIG_MATH_FPU_RECIP
/* Positive normal inputs need no subnormal fallback. */
static inline __attribute__((always_inline)) float
mathf_rsqrt_normal(float value) {
    float seed;
    __asm__("rsqrt0.s %0, %1" : "=f"(seed) : "f"(value));
    seed += (0.5F * seed) * (1.0F - (value * seed) * seed);
    return seed + (0.5F * seed) * (1.0F - (value * seed) * seed);
}
#endif

/* Subnormal squared lengths retain the 1 / sqrtf path. On the board +inf gives NaN, outside normalize's domain. */
/* For positive input v, reciprocal-root seed y with two Newton corrections. | y = y + (y/2)(1 - vy^2) | all mantissas in [1,4), normal exponent strides and edges */

/* source: Muller et al. 2018 */
/* exact: y = 1/\sqrt{v} */
/* MATH_FAST_PATH(mathf_rsqrt_f, div_rsqrt_f, MATHF_RSQRT_ULP_LIMIT) */

static inline __attribute__((always_inline)) float
mathf_rsqrt(float value) {
#if defined(__XTENSA__) && CONFIG_MATH_FPU_RECIP
    if (value < FLT_MIN) {
        return 1.0F / sqrtf(value);
    }
    return mathf_rsqrt_normal(value);
#else
    return 1.0F / sqrtf(value);
#endif
}

static inline float
mathf_sqrt(float a) {
    return sqrtf(a);
}

static inline float
mathf_half_sin(float angle) {
    return sinf(angle * 0.5F);
}

static inline float
mathf_half_cos(float angle) {
    return cosf(angle * 0.5F);
}

/* a0 * b0 + a1 * b1 + c, summed left to right. */
static inline float
mathf_dot2c(float a0, float b0, float a1, float b1, float c) {
    return ((a0 * b0) + (a1 * b1)) + c;
}

/* a0 * b0 + a1 * b1 + a2 * b2 + c, summed left to right. */
static inline float
mathf_dot3c(float a0, float b0, float a1, float b1, float a2, float b2, float c) {
    return (((a0 * b0) + (a1 * b1)) + (a2 * b2)) + c;
}

static inline float
mathf_wadd(float a, float b) {
    return a + b;
}

static inline float
mathf_wmul(float a, float b) {
    return a * b;
}

static inline float
mathf_lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

static inline float
mathf_min(float a, float b) {
    return a < b ? a : b;
}

static inline float
mathf_max(float a, float b) {
    return a < b ? b : a;
}

/* Reciprocal and product rounding must leave the floor within one integer. */
#define MATHF_REPEAT_FAST_QUOTIENT 0x1p22F
_Static_assert((MATHF_RECIP_ULP_LIMIT + 0.5F) * MATHF_REPEAT_FAST_QUOTIENT * FLT_EPSILON < 1.0F,
               "repeat quotient guard exceeds the reciprocal error bound");

/* For input v and positive period len, reciprocal quotient and fused remainder r, with range fallback. | r = fma(-floor(v/len), len, v) | signed float bit patterns, quotient guard and endpoint edges */

/* Exactness for the floor quotient follows from 0 <= r < len. */
/* source: Muller et al. 2018 */
/* exact: r = v - \lfloor v/len \rfloor len */
/* MATH_FAST_PATH(wrap_f, wrap_fmodf_f, MATHF_WRAP_ULP_LIMIT=0 ulp) */

static inline float
mathf_repeat(float v, float len) {
    assert(len > 0.0F);
    if (v >= 0.0F && v < len) {
        return v;
    }
    /* QEMU's msub.s rounds the product, so the fused remainder is not exact there. */
#if !defined(__XTENSA__) || CONFIG_MATH_FPU_FUSED_MADD
    const float reciprocal = mathf_recip(len);
    const float quotient = v * reciprocal;
    if (fabsf(quotient) < MATHF_REPEAT_FAST_QUOTIENT && len <= FLT_MAX) {
        float q = (float)(int32_t)quotient;
        if (q > quotient) {
            q -= 1.0F;
        }
        /* For the correct floor, fmaf rounds v - q*len once: the exact fmodf
         * remainder for v >= 0, or the same rounding as len - |fmodf| for v < 0. */
        float rem = fmaf(-q, len, v);
        if (rem < 0.0F) {
            rem = fmaf(-(q - 1.0F), len, v);
        } else if (rem >= len) {
            rem = fmaf(-(q + 1.0F), len, v);
            /* A negative correction means the remainder rounded onto the excluded endpoint. */
            if (rem < 0.0F) {
                return 0.0F;
            }
        }
        /* fmodf preserves the dividend's sign on exact multiples. */
        if (rem == 0.0F) {
            return copysignf(0.0F, v);
        }
        return rem < len ? rem : 0.0F;
    }
#endif
    float r = fmodf(v, len);
    if (r < 0.0F) {
        r += len;
    }
    /* A tiny negative input can round the remainder up to the excluded endpoint. */
    return r < len ? r : 0.0F;
}

static inline float
mathf_wrap(float v, float lo, float hi) {
    assert(lo < hi);
    const float wrapped = lo + mathf_repeat(v - lo, hi - lo);
    return wrapped < hi ? wrapped : lo;
}
