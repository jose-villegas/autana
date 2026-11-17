/*
 * mathx: Q16.16 fixed-point scalar operations for the math templates
 * (docs/math/Scalar.md).
 *
 * Operations saturate, except the mathx_dot2c(), mathx_dot3c() and
 * mathx_dot3_narrow() fast paths, which wrap and floor; saturation turns
 * overflow into a bounded coordinate. A divide by zero saturates by the
 * numerator's sign (0 / 0 is 0). Other products round to nearest, ties away
 * from zero. Angles are Q16.16 turns whose low 16 bits are math/scalar/trig.h phases;
 * a sine or cosine of +-1.0 comes back as +-65534. Header-only, static
 * inline and ESP-IDF-free.
 */
#pragma once

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

#include "math/scalar/fixed.h"
#include "math/scalar/mathf.h"
#include "math/scalar/mathf_round.h"
#include "math/scalar/mathi.h"
#include "math/scalar/trig.h"

#define MATHX_SHIFT 16
#define MATHX_ONE   (1 << MATHX_SHIFT)

static inline int32_t
mathf_to_x(float v) {
    return mathf_round_i32(v * (float)MATHX_ONE);
}

static inline float
mathx_to_f(int32_t v) {
    return (float)v * (1.0F / (float)MATHX_ONE);
}

static inline int32_t
mathx_saturate(int64_t v) {
    if (v > INT32_MAX) {
        return INT32_MAX;
    }
    return v < INT32_MIN ? INT32_MIN : (int32_t)v;
}

static inline int32_t
mathx_add(int32_t a, int32_t b) {
    return mathx_saturate((int64_t)a + b);
}

static inline int32_t
mathx_sub(int32_t a, int32_t b) {
    return mathx_saturate((int64_t)a - b);
}

static inline int32_t
mathx_neg(int32_t a) {
    return mathx_saturate(-(int64_t)a);
}

static inline int32_t
mathx_mul(int32_t a, int32_t b) {
    return mathx_saturate(fx_round_shift((int64_t)a * b, MATHX_SHIFT));
}

static inline int32_t
mathx_div(int32_t num, int32_t den) {
    if (den == 0) {
        return num > 0 ? INT32_MAX : (num < 0 ? INT32_MIN : 0);
    }
    return mathx_saturate(fx_div_round_wide(num, den, MATHX_SHIFT));
}

/* The square root of a non-negative value; a negative one gives 0. */
static inline int32_t
mathx_sqrt(int32_t v) {
    if (v <= 0) {
        return 0;
    }
    return (int32_t)mathi_isqrt64((uint64_t)v << MATHX_SHIFT);
}

_Static_assert(TRIG_TURN == MATHX_ONE, "Q16.16 turns match trig phases");

static inline int32_t
mathx_sin_turns(int32_t turns) {
    return trig_sin((uint16_t)turns) * (1 << (MATHX_SHIFT - TRIG_SIN_SHIFT));
}

static inline int32_t
mathx_cos_turns(int32_t turns) {
    return trig_cos((uint16_t)turns) * (1 << (MATHX_SHIFT - TRIG_SIN_SHIFT));
}

/* The dot products a matrix transform sums, as a fast path: each product is
 * floored to Q16.16 (a multiply and a funnel shift, no 64-bit sum, no sign
 * branch) and the 32-bit sum WRAPS, unlike mathx_add. Each term is off by
 * under one unit, so a sum by under three. For coordinates the caller keeps
 * inside +-32768, which a camera and a floor are. */
static inline int32_t
mathx_dot2c(int32_t a0, int32_t b0, int32_t a1, int32_t b1, int32_t c) {
    return (int32_t)((uint32_t)fx_mul_floor(a0, b0, MATHX_SHIFT) + (uint32_t)fx_mul_floor(a1, b1, MATHX_SHIFT)
                     + (uint32_t)c);
}

static inline int32_t
mathx_dot3c(int32_t a0, int32_t b0, int32_t a1, int32_t b1, int32_t a2, int32_t b2, int32_t c) {
    return (int32_t)((uint32_t)fx_mul_floor(a0, b0, MATHX_SHIFT) + (uint32_t)fx_mul_floor(a1, b1, MATHX_SHIFT)
                     + (uint32_t)fx_mul_floor(a2, b2, MATHX_SHIFT) + (uint32_t)c);
}

/* a0 * b0 + a1 * b1 + a2 * b2 + c, then an arithmetic shift right by `shift`:
 * plain 32-bit products and sum, a multiply and an add each, for operands
 * whose whole sum fits in an int32. The caller proves that range; nothing
 * here saturates. */
static inline int32_t
mathx_dot3_narrow(int32_t a0, int32_t b0, int32_t a1, int32_t b1, int32_t a2, int32_t b2, int32_t c, int shift) {
    const uint32_t sum =
        ((uint32_t)a0 * (uint32_t)b0) + ((uint32_t)a1 * (uint32_t)b1) + ((uint32_t)a2 * (uint32_t)b2) + (uint32_t)c;
    return (int32_t)sum >> shift;
}

static inline int32_t
mathx_zero(void) {
    return 0;
}

static inline int32_t
mathx_one(void) {
    return MATHX_ONE;
}

#define MATHX_RSQRT_SEED_BITS               6
#define MATHX_RSQRT_WORD_BITS               32
#define MATHX_RSQRT_SHIFT                   30
#define MATHX_RSQRT_MIN                     (UINT32_C(1) << MATHX_RSQRT_SHIFT)
#define MATHX_RSQRT_Q30_RELATIVE_LIMIT      0x1p-31
#define MATHX_RSQRT_NEWTON_STEPS            3
/* The float seed is within 2^-22; this wider bound also covers integer conversion. */
#define MATHX_RSQRT_FPU_SEED_RELATIVE_LIMIT 0x1p-20
#define MATHX_RSQRT_FPU_INPUT_SCALE         (1.0F / (float)(UINT64_C(1) << MATHX_RSQRT_WORD_BITS))
#define MATHX_RSQRT_FPU_OUTPUT_SCALE        ((float)(UINT32_C(1) << MATHX_RSQRT_SHIFT))

/* Upper 64 bits of the 96-bit product; the omitted low word cannot affect a right shift by 60. */
static inline uint64_t
mathx_rsqrt_product(uint32_t m, uint64_t square) {
    const uint64_t low = (uint64_t)m * (uint32_t)square;
    return (uint64_t)m * (square >> MATHX_RSQRT_WORD_BITS) + (low >> MATHX_RSQRT_WORD_BITS);
}

/* Exact midpoint bracket: m * (y + 1/2)^2 <= 2^92, without a 96-bit type. */
static inline bool
mathx_rsqrt_midpoint(uint32_t m, uint32_t y) {
    const uint64_t square = (uint64_t)y * (y + UINT64_C(1));
    const uint64_t product = (uint64_t)m * (uint32_t)square;
    const uint64_t low = product + (((uint64_t)m + 3) >> 2);
    const uint64_t high = (uint64_t)m * (square >> MATHX_RSQRT_WORD_BITS) + (low >> MATHX_RSQRT_WORD_BITS)
                          + ((uint64_t)(low < product) << MATHX_RSQRT_WORD_BITS);
    const uint64_t target = UINT64_C(1) << (2 * MATHX_RSQRT_SHIFT);
    return high < target || (high == target && (uint32_t)low == 0);
}

/* Entry i = round(2^30 / sqrt((16 + i + 0.5) / 64)), the midpoint of mantissa segment 16 + i. */
static const uint32_t mathx_rsqrt_seeds[] = {
    [0] = 2114695713u,  [1] = 2053387115u,  [2] = 1997119227u,  [3] = 1945237133u,  [4] = 1897199172u,
    [5] = 1852552937u,  [6] = 1810917218u,  [7] = 1771968208u,  [8] = 1735428857u,  [9] = 1701060526u,
    [10] = 1668656406u, [11] = 1638036256u, [12] = 1609042172u, [13] = 1581535151u, [14] = 1555392273u,
    [15] = 1530504391u, [16] = 1506774204u, [17] = 1484114654u, [18] = 1462447584u, [19] = 1441702596u,
    [20] = 1421816090u, [21] = 1402730445u, [22] = 1384393311u, [23] = 1366757007u, [24] = 1349778000u,
    [25] = 1333416450u, [26] = 1317635818u, [27] = 1302402522u, [28] = 1287685637u, [29] = 1273456629u,
    [30] = 1259689126u, [31] = 1246358707u, [32] = 1233442724u, [33] = 1220920139u, [34] = 1208771378u,
    [35] = 1196978204u, [36] = 1185523604u, [37] = 1174391680u, [38] = 1163567563u, [39] = 1153037323u,
    [40] = 1142787899u, [41] = 1132807028u, [42] = 1123083182u, [43] = 1113605518u, [44] = 1104363818u,
    [45] = 1095348453u, [46] = 1086550331u, [47] = 1077960865u,
};

static inline uint32_t
mathx_rsqrt_newton(uint32_t m, uint32_t y) {
    const uint64_t product = mathx_rsqrt_product(m, (uint64_t)y * y);
    const int64_t residual =
        (INT64_C(1) << MATHX_RSQRT_WORD_BITS) - (int64_t)(product >> (2 * MATHX_RSQRT_SHIFT - MATHX_RSQRT_WORD_BITS));
    return (
        uint32_t)((int64_t)y
                  + (((int64_t)y * residual + (INT64_C(1) << MATHX_RSQRT_WORD_BITS)) >> (MATHX_RSQRT_WORD_BITS + 1)));
}

static inline uint32_t
mathx_rsqrt_bracket(uint32_t m, uint32_t y) {
    y += mathx_rsqrt_midpoint(m, y);
    y -= !mathx_rsqrt_midpoint(m, y - 1);
    return y;
}

static inline uint32_t
mathx_rsqrt_q30_table(uint32_t m) {
    assert(m >= MATHX_RSQRT_MIN);
    _Static_assert(sizeof mathx_rsqrt_seeds / sizeof mathx_rsqrt_seeds[0]
                       == (1 << MATHX_RSQRT_SEED_BITS) - (1 << (MATHX_RSQRT_SEED_BITS - 2)),
                   "Every mantissa segment has a reciprocal seed");
    const unsigned index = (m >> (MATHX_RSQRT_WORD_BITS - MATHX_RSQRT_SEED_BITS)) - (1 << (MATHX_RSQRT_SEED_BITS - 2));
    uint32_t y = mathx_rsqrt_seeds[index];
    for (int step = 0; step < MATHX_RSQRT_NEWTON_STEPS; step++) {
        y = mathx_rsqrt_newton(m, y);
    }
    return mathx_rsqrt_bracket(m, y);
}

/* For m in [2^30, 2^32), midpoint rounding bounds relative error by 2^-31.
 * Uses the FPU: FreeRTOS lazy FPU save pins the calling task to its core; not ISR-safe. */
/* For integer mantissa m in [MATHX_RSQRT_MIN, 2^32), an FPU seed plus one integer Newton correction (table fallback) and exact midpoint bracket give reciprocal root y. | y = round(2^{46}/sqrt(m)) | mantissa segment strides and exact midpoint brackets */

/* source: Muller et al. 2018; Markstein 1990 */
/* exact: y = 2^{46}/\sqrt{m} */
/* FAST_PATH(normalize_x, normalize_div_x, MATHX_RSQRT_Q30_RELATIVE_LIMIT) */

/* source: Muller et al. 2018; Markstein 1990 */
/* exact: y = 2^{46}/\sqrt{m} */
/* FAST_PATH(nlerp_x, nlerp_div_x, MATHX_RSQRT_Q30_RELATIVE_LIMIT) */

static inline uint32_t
mathx_rsqrt_q30(uint32_t m) {
#if defined(__XTENSA__) && CONFIG_MATH_FPU_RECIP
    assert(m >= MATHX_RSQRT_MIN);
    const float x = (float)m * MATHX_RSQRT_FPU_INPUT_SCALE;
    const float r = mathf_rsqrt_normal(x);
    const uint32_t seed = (uint32_t)(r * MATHX_RSQRT_FPU_OUTPUT_SCALE);
    return mathx_rsqrt_bracket(m, mathx_rsqrt_newton(m, seed));
#else
    return mathx_rsqrt_q30_table(m);
#endif
}

#define MATHX_RSQRT_RELATIVE_LIMIT 0x1p-30

/* Within 0.5 LSB plus relative 2^-30 of Q16.16 1/sqrt(v); v <= 0 gives INT32_MAX. */
static inline int32_t
mathx_rsqrt(int32_t v) {
    if (v <= 0) {
        return INT32_MAX;
    }
    const int shift = __builtin_clz((uint32_t)v) & ~1;
    const uint32_t root = mathx_rsqrt_q30((uint32_t)v << shift);
    const int output_shift = MATHX_RSQRT_SHIFT + MATHX_RSQRT_WORD_BITS / 2 - MATHX_SHIFT - MATHX_SHIFT / 2 - shift / 2;
    return (int32_t)fx_round_shift(root, output_shift);
}

static inline int32_t
mathx_half(void) {
    return MATHX_ONE / 2;
}

static inline int32_t
mathx_abs(int32_t a) {
    return a < 0 ? mathx_neg(a) : a;
}

static inline int32_t
mathx_two(void) {
    return 2 * MATHX_ONE;
}

static inline int32_t
mathx_four(void) {
    return 4 * MATHX_ONE;
}

static inline int32_t
mathx_half_sin(int32_t turns) {
    return mathx_sin_turns(turns / 2);
}

static inline int32_t
mathx_half_cos(int32_t turns) {
    return mathx_cos_turns(turns / 2);
}

static inline int32_t
mathx_wadd(int32_t a, int32_t b) {
    return mathx_add(a, b);
}

static inline int32_t
mathx_wmul(int32_t a, int32_t b) {
    return mathx_mul(a, b);
}

static inline int32_t
mathx_lerp(int32_t a, int32_t b, int32_t t) {
    return mathx_add(a, mathx_mul(mathx_sub(b, a), t));
}

static inline int32_t
mathx_min(int32_t a, int32_t b) {
    return mathi_min(a, b);
}

static inline int32_t
mathx_max(int32_t a, int32_t b) {
    return mathi_max(a, b);
}

static inline int32_t
mathx_clamp(int32_t v, int32_t lo, int32_t hi) {
    return mathi_clamp(v, lo, hi);
}

static inline int32_t
mathx_repeat(int32_t v, int32_t len) {
    return mathi_repeat(v, len);
}

static inline int32_t
mathx_wrap(int32_t v, int32_t lo, int32_t hi) {
    return mathi_wrap(v, lo, hi);
}

/* n <= 4. Relative budget in 2^-30 units for scaled length L:
 * L < 2^30: prescale <= sqrt(n), no reciprocal rounding.
 * L >= 2^30: prescale <= 1, reciprocal rounding <= 1.
 * Both: sum truncation <= 0.5, root <= 0.5; total < 4 = 2^-28.
 * Components: 0.5 + 2^-12 LSB. Quotient: relative 2^-28 + 0.5 LSB, saturated.
 * Zero input gives zero and mathx_div(t, 0). */
static inline void
mathx_normalize_n(const int32_t* in, int32_t* out, size_t n, int32_t t, int32_t* q) {
    enum {
        MATHX_NORMALIZE_MAX_COMPONENTS = 4,
        MATHX_NORMALIZE_COMPONENT_SHIFT = 29,
        MATHX_NORMALIZE_TOP_BIT = 31,
        MATHX_NORMALIZE_RECIPROCAL_SHIFT = MATHX_NORMALIZE_COMPONENT_SHIFT + MATHX_NORMALIZE_TOP_BIT,
        MATHX_NORMALIZE_OUTPUT_SHIFT = MATHX_NORMALIZE_RECIPROCAL_SHIFT - MATHX_SHIFT,
    };

    _Static_assert((UINT64_C(1) << (2 * (MATHX_NORMALIZE_COMPONENT_SHIFT + 1)))
                       <= UINT64_MAX / MATHX_NORMALIZE_MAX_COMPONENTS,
                   "Four scaled component squares fit uint64");
    _Static_assert((UINT64_C(1) << (MATHX_NORMALIZE_COMPONENT_SHIFT + 1)) * INT32_MAX <= (UINT64_C(1) << 62),
                   "Scaled component times reciprocal fits with rounding headroom");
    _Static_assert((UINT64_C(1) << MATHX_NORMALIZE_TOP_BIT) * INT32_MAX <= (UINT64_C(1) << 62),
                   "Numerator times reciprocal fits with rounding headroom");
    assert(n > 0 && n <= MATHX_NORMALIZE_MAX_COMPONENTS);
    uint32_t m = 0;
    for (size_t i = 0; i < n; i++) {
        const uint32_t magnitude = in[i] < 0 ? (uint32_t)-(int64_t)in[i] : (uint32_t)in[i];
        if (magnitude > m) {
            m = magnitude;
        }
    }
    if (m == 0) {
        *q = mathx_div(t, 0);
        for (size_t i = 0; i < n; i++) {
            out[i] = 0;
        }
        return;
    }
    const int s = MATHX_NORMALIZE_COMPONENT_SHIFT - (MATHX_NORMALIZE_TOP_BIT - __builtin_clz(m));
    uint64_t sum = 0;
    for (size_t i = 0; i < n; i++) {
        out[i] = (int32_t)(s >= 0 ? (int64_t)in[i] * ((int64_t)1 << s) : fx_round_shift(in[i], -s));
        sum += (uint64_t)((int64_t)out[i] * out[i]);
    }
    const int exponent = (2 * MATHX_RSQRT_WORD_BITS - 1 - __builtin_clzll(sum)) & ~1;
    const int mantissa_shift = exponent - MATHX_RSQRT_SHIFT;
    const uint32_t mantissa = (uint32_t)(mantissa_shift >= 0 ? sum >> mantissa_shift : sum << -mantissa_shift);
    const uint32_t root = mathx_rsqrt_q30(mantissa);
    const int reciprocal_shift = exponent / 2 + 1 - MATHX_RSQRT_SHIFT;
    assert(reciprocal_shift >= 0 && reciprocal_shift <= 2);
    const uint64_t reciprocal = reciprocal_shift > 0 ? (uint64_t)fx_round_shift(root, reciprocal_shift) : root;
    const int32_t k = mathx_saturate((int64_t)reciprocal);
    *q = mathx_saturate(fx_round_shift((int64_t)t * k, MATHX_NORMALIZE_OUTPUT_SHIFT - s));
    for (size_t i = 0; i < n; i++) {
        out[i] = (int32_t)fx_round_shift((int64_t)out[i] * k, MATHX_NORMALIZE_OUTPUT_SHIFT);
    }
}
