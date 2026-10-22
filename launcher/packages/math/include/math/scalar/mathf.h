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

/* 1 / a for a nonzero a away from float's range limits. The S3's `/` is a
 * libgcc routine of about 56 cycles; its FPU's reciprocal seed and two Newton
 * steps take about 18 and land within one ulp of the quotient, on it 99.9% of
 * the time. CONFIG_MATH_FPU_RECIP enables the seed; without it, division
 * is exact, as on other targets. */
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

/* Subnormal squared lengths retain the 1 / sqrtf path. On the board +inf gives NaN, outside normalize's domain. */
static inline __attribute__((always_inline)) float
mathf_rsqrt(float value) {
#if defined(__XTENSA__) && CONFIG_MATH_FPU_RECIP
    if (value < FLT_MIN) {
        return 1.0F / sqrtf(value);
    }
    float seed;
    __asm__("rsqrt0.s %0, %1" : "=f"(seed) : "f"(value));
    seed += (0.5F * seed) * (1.0F - (value * seed) * seed);
    return seed + (0.5F * seed) * (1.0F - (value * seed) * seed);
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

/* The reciprocal bound keeps the estimated floor within one integer. */
#define MATHF_REPEAT_FAST_QUOTIENT 0x1p22F

static inline float
mathf_repeat(float v, float len) {
    assert(len > 0.0F);
    if (v >= 0.0F && v < len) {
        return v;
    }
    const float reciprocal = mathf_recip(len);
    const float quotient = v * reciprocal;
    if (fabsf(quotient) < MATHF_REPEAT_FAST_QUOTIENT) {
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
