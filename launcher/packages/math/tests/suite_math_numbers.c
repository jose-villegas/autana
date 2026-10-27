/*
 * Portable suite: math/linear/'s number types. The same operations run on
 * float, int32, int16 and Q16.16 vectors, and the fixed-point rotation maths
 * on Q16.16, and each is judged against the float result within its own
 * precision. Conversions round-trip, and overflow saturates where the
 * headers say it does.
 */

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "testkit/suite.h"
#include "transform_cache.h"
#include "unity.h"

#include "math/linear/transformf.h"
#include "math/linear/transformx.h"
#include "math/linear/vec_convert.h"
#include "math/scalar/math_const.h"
#include "math/scalar/mathf.h"
#include "math/scalar/mathi.h"
#include "math/scalar/units.h"

#define NUMBER_LCG_MULTIPLIER UINT32_C(1664525)
#define NUMBER_LCG_INCREMENT  UINT32_C(1013904223)

#define Q                     ((float)MATHX_ONE)
/* Q16.16's table-driven sine is good to about 1e-3; sums of products to far
 * less, so rotation results are compared at this. */
#define FIXED_SLACK           2e-3F

static vec3x_t
to_x(float x, float y, float z) {
    return vec3x_from_vec3f((vec3f_t){x, y, z});
}

static void
assert_x_near_f(vec3f_t want, vec3x_t got, float slack) {
    const vec3f_t g = vec3f_from_vec3x(got);
    TEST_ASSERT_FLOAT_WITHIN(slack, want.x, g.x);
    TEST_ASSERT_FLOAT_WITHIN(slack, want.y, g.y);
    TEST_ASSERT_FLOAT_WITHIN(slack, want.z, g.z);
}

/* One pair of whole-number vectors, so every type holds them exactly. */
static const vec3f_t A = {3.0F, -2.0F, 5.0F};
static const vec3f_t B = {1.0F, 4.0F, -6.0F};

static void
test_looking_keeps_forward_at_large_positions(void) {
    const vec3f_t forward = {1.0F, 0.0F, 0.0F};
    const vec3f_t up = {0.0F, 1.0F, 0.0F};
    const vec3f_t far = {1e9F, 1e9F, 1e9F};
    const transformf_t pose = transformf_looking(far, forward, up);
    const vec3f_t got = quatf_rotate(pose.rotation, (vec3f_t){0.0F, 0.0F, 1.0F});
    TEST_ASSERT_FLOAT_WITHIN(FIXED_SLACK, forward.x, got.x);
    TEST_ASSERT_FLOAT_WITHIN(FIXED_SLACK, forward.y, got.y);
    TEST_ASSERT_FLOAT_WITHIN(FIXED_SLACK, forward.z, got.z);
    TEST_ASSERT_EQUAL_FLOAT(far.x, pose.position.x);
    const transformx_t fixed =
        transformx_looking(to_x(30000.0F, 0.0F, 0.0F), to_x(1.0F, 0.0F, 0.0F), to_x(0.0F, 1.0F, 0.0F));
    assert_x_near_f(forward, quatx_rotate(fixed.rotation, to_x(0.0F, 0.0F, 1.0F)), FIXED_SLACK);
}

static void
test_vec3_add_sub_scale_dot_cross_agree_across_every_number_type(void) {
    const vec3f_t sum = vec3f_add(A, B);
    const vec3f_t cross = vec3f_cross(A, B);
    const float dot = vec3f_dot(A, B);

    const vec3i_t ai = {3, -2, 5};
    const vec3i_t bi = {1, 4, -6};
    const vec3f_t sum_i = vec3f_from_vec3i(vec3i_add(ai, bi));
    TEST_ASSERT_EQUAL_FLOAT(sum.x, sum_i.x);
    TEST_ASSERT_EQUAL_FLOAT(sum.z, sum_i.z);
    TEST_ASSERT_EQUAL_FLOAT(cross.y, (float)vec3i_cross(ai, bi).y);
    TEST_ASSERT_EQUAL_FLOAT(dot, (float)vec3i_dot(ai, bi));

    const vec3s_t as = vec3s_from_vec3i(ai);
    const vec3s_t bs = vec3s_from_vec3i(bi);
    TEST_ASSERT_EQUAL_FLOAT(sum.y, vec3f_from_vec3s(vec3s_add(as, bs), 1.0F).y);
    TEST_ASSERT_EQUAL_FLOAT(cross.x, vec3f_from_vec3s(vec3s_cross(as, bs), 1.0F).x);
    TEST_ASSERT_EQUAL_FLOAT(dot, (float)vec3s_dot(as, bs));
    TEST_ASSERT_EQUAL_INT16(33, vec3s_scale(vec3s_sub(as, bs), 3).z);

    const vec3x_t ax = vec3x_from_vec3f(A);
    const vec3x_t bx = vec3x_from_vec3f(B);
    assert_x_near_f(sum, vec3x_add(ax, bx), 1e-6F);
    assert_x_near_f(cross, vec3x_cross(ax, bx), 1e-6F);
    TEST_ASSERT_FLOAT_WITHIN(1e-6F, dot, (float)vec3x_dot(ax, bx) / Q);
    assert_x_near_f(vec3f_scale(A, 0.5F), vec3x_scale(ax, MATHX_ONE / 2), 1e-6F);
}

static void
test_vec2_ops_agree_across_every_number_type(void) {
    const vec2f_t a = {3.0F, -2.0F};
    const vec2f_t b = {1.0F, 4.0F};
    const vec2f_t sum = vec2f_add(a, b);
    TEST_ASSERT_EQUAL_FLOAT(sum.y, (float)vec2i_add((vec2i_t){3, -2}, (vec2i_t){1, 4}).y);
    TEST_ASSERT_EQUAL_INT(2, vec2s_add((vec2s_t){3, -2}, (vec2s_t){1, 4}).y);
    TEST_ASSERT_EQUAL_FLOAT(vec2f_dot(a, b), (float)vec2i_dot((vec2i_t){3, -2}, (vec2i_t){1, 4}));
    const vec2x_t ax = vec2x_from_vec2f(a);
    TEST_ASSERT_FLOAT_WITHIN(1e-6F, vec2f_dot(a, b), mathx_to_f(vec2x_dot(ax, vec2x_from_vec2f(b))));
    TEST_ASSERT_TRUE(vec2i_equal((vec2i_t){1, 2}, (vec2i_t){1, 2}));
    TEST_ASSERT_FALSE(vec2s_equal((vec2s_t){1, 2}, (vec2s_t){1, 3}));
}

/* The double-precision reference runs on the host only: the device's FPU is single precision. */
#ifndef ESP_PLATFORM
#define NORMALIZE_ROUNDING_ERROR          0.5
#define NORMALIZE_COMPONENT_ERROR         (NORMALIZE_ROUNDING_ERROR + 0x1p-12)

/* mathx_normalize_n() documents the shared component and quotient error budget. */
#define NORMALIZE_QUOTIENT_RELATIVE_ERROR 0x1p-28
#define NORMALIZE_WORD_BITS               32
#define NORMALIZE_SWEEP_SAMPLES           2048

static void
assert_normalize_quotient_reference(vec3x_t a, int32_t t) {
    const double length = sqrt((double)a.x * a.x + (double)a.y * a.y + (double)a.z * a.z);
    const double exact = (double)t * MATHX_ONE / length;
    const double expected = fmax(INT32_MIN, fmin(INT32_MAX, exact));
    int32_t q;
    vec3x_normalize_divide(a, t, &q);
    TEST_ASSERT_TRUE(fabs(q - expected)
                     <= NORMALIZE_ROUNDING_ERROR + fabs(expected) * NORMALIZE_QUOTIENT_RELATIVE_ERROR);
}

static void
assert_normalize_reference(vec3x_t a) {
    const double length = sqrt((double)a.x * a.x + (double)a.y * a.y + (double)a.z * a.z);
    const vec3x_t out = vec3x_normalize(a);
    static const int32_t numerators[] = {0, 1, -1, MATHX_ONE, -MATHX_ONE, INT32_MAX, INT32_MIN};
    for (size_t i = 0; i < sizeof(numerators) / sizeof(numerators[0]); i++) {
        assert_normalize_quotient_reference(a, numerators[i]);
    }
    TEST_ASSERT_TRUE(fabs(out.x - (double)a.x * MATHX_ONE / length) <= NORMALIZE_COMPONENT_ERROR);
    TEST_ASSERT_TRUE(fabs(out.y - (double)a.y * MATHX_ONE / length) <= NORMALIZE_COMPONENT_ERROR);
    TEST_ASSERT_TRUE(fabs(out.z - (double)a.z * MATHX_ONE / length) <= NORMALIZE_COMPONENT_ERROR);
}

#endif

static void
test_normalize_divide_zero_and_float(void) {
    static const int32_t numerators[] = {INT32_MIN, -1, 0, 1, INT32_MAX};
    const vec3x_t zero = {0, 0, 0};
    for (size_t i = 0; i < sizeof(numerators) / sizeof(numerators[0]); i++) {
        int32_t q;
        TEST_ASSERT_TRUE(vec3x_equal(zero, vec3x_normalize_divide(zero, numerators[i], &q)));
        TEST_ASSERT_EQUAL_INT32(mathx_div(numerators[i], 0), q);
    }
    const vec3f_t normalized = vec3f_normalize(A);
    static const float float_numerators[] = {B.x, -B.x, 0.0F};
    for (size_t i = 0; i < sizeof(float_numerators) / sizeof(float_numerators[0]); i++) {
        float q;
        const vec3f_t divided = vec3f_normalize_divide(A, float_numerators[i], &q);
        TEST_ASSERT_TRUE(vec3f_equal(normalized, divided));
        TEST_ASSERT_FLOAT_WITHIN(FLT_EPSILON, float_numerators[i] / sqrtf(vec3f_dot(A, A)), q);
    }
}

/* The double-precision reference runs on the host only: the device's FPU is single precision. */
#ifndef ESP_PLATFORM
static void
test_fixed_normalize_saturation_edges(void) {
    static const struct {
        vec3x_t a;
        int32_t t;
    } oblique_cases[] = {{{-19370, 18651, -10733}, -948719331}, {{-25384, -6545, 25263}, 1192954874}};

    for (size_t i = 0; i < sizeof(oblique_cases) / sizeof(oblique_cases[0]); i++) {
        assert_normalize_quotient_reference(oblique_cases[i].a, oblique_cases[i].t);
    }

    enum { EDGE_RADIUS = 2, LENGTH_NUMERATOR = 3, LENGTH_DENOMINATOR = 4 };

    const vec3x_t a = {LENGTH_NUMERATOR * (MATHX_ONE / LENGTH_DENOMINATOR), 0, 0};
    for (int offset = -EDGE_RADIUS; offset <= EDGE_RADIUS; offset++) {
        for (int sign = -1; sign <= 1; sign += 2) {
            const int32_t t = sign * LENGTH_NUMERATOR * (INT32_MAX / LENGTH_DENOMINATOR + offset);
            assert_normalize_quotient_reference(a, t);
        }
    }
}

#endif

static void
test_fixed_normalize_extreme_magnitudes(void) {
    static const vec3x_t cases[] = {{200 * MATHX_ONE, 0, 0},           {1, 0, 0},         {1, 1, 0}, {1, 1, 1},
                                    {INT32_MIN, INT32_MIN, INT32_MIN}, {INT32_MAX, -5, 3}};
#ifndef ESP_PLATFORM
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        assert_normalize_reference(cases[i]);
    }
#endif
    TEST_ASSERT_EQUAL_INT32(MATHX_ONE, vec3x_normalize(cases[0]).x);
    TEST_ASSERT_EQUAL_INT32(MATHX_ONE, vec3x_normalize(cases[1]).x);
    const vec3x_t negative_axis = {INT32_MIN, 0, 0};
    int32_t q;
    TEST_ASSERT_TRUE(vec3x_equal((vec3x_t){-MATHX_ONE, 0, 0}, vec3x_normalize_divide(negative_axis, INT32_MIN, &q)));
    TEST_ASSERT_EQUAL_INT32(-MATHX_ONE, q);
    vec3x_normalize_divide(cases[1], MATHX_ONE, &q);
    TEST_ASSERT_EQUAL_INT32(INT32_MAX, q);
    vec3x_normalize_divide(cases[1], -MATHX_ONE, &q);
    TEST_ASSERT_EQUAL_INT32(INT32_MIN, q);
}

/* The double-precision reference runs on the host only: the device's FPU is single precision. */
#ifndef ESP_PLATFORM
static void
test_fixed_normalize_every_magnitude(void) {
    uint32_t state = 1;
    for (int shift = 0; shift < NORMALIZE_WORD_BITS; shift++) {
        for (int sample = 0; sample < NORMALIZE_SWEEP_SAMPLES; sample++) {
            int32_t components[3];
            for (size_t i = 0; i < sizeof(components) / sizeof(components[0]); i++) {
                state = state * NUMBER_LCG_MULTIPLIER + NUMBER_LCG_INCREMENT;
                const int component_shift = (shift + (int)(state >> (NORMALIZE_WORD_BITS / 2))) % NORMALIZE_WORD_BITS;
                const int64_t magnitude = (state >> (NORMALIZE_WORD_BITS - 1 - component_shift)) | 1;
                components[i] = mathx_saturate((state & 1) ? -magnitude : magnitude);
            }
            assert_normalize_reference((vec3x_t){components[0], components[1], components[2]});
        }
    }
}

#endif

static void
test_fixed_normalize_matches_float_within_its_precision(void) {
    const vec3f_t n = vec3f_normalize(A);
    assert_x_near_f(n, vec3x_normalize(vec3x_from_vec3f(A)), 1e-3F);
}

static void
test_fixed_quaternion_rotation_matches_float(void) {
    /* An eighth, a twelfth and a sixth of a turn, in turns and in radians. */
    const vec3x_t turns = {MATHX_ONE / 8, MATHX_ONE / 12, MATHX_ONE / 6};
    const vec3f_t radians = {MATH_TAU / 8.0F, MATH_TAU / 12.0F, MATH_TAU / 6.0F};
    const vec3f_t v = {1.0F, 2.0F, 3.0F};

    const quatx_t qx = quatx_from_euler(turns);
    const quatf_t qf = quatf_from_euler(radians);
    assert_x_near_f(quatf_rotate(qf, v), quatx_rotate(qx, vec3x_from_vec3f(v)), FIXED_SLACK);
}

static void
test_fixed_view_from_axes_maps_the_eye_and_frame_axes(void) {
    const vec3x_t x = {0, MATHX_ONE, 0}, y = {0, 0, -MATHX_ONE}, z = {-MATHX_ONE, 0, 0};
    const vec3x_t eye = {2 * MATHX_ONE, -3 * MATHX_ONE, 4 * MATHX_ONE}, origin = {0, 0, 0};
    const mat4x_t view = mat4x_view_from_axes(x, y, z, eye);
    TEST_ASSERT_TRUE(vec3x_equal(origin, mat4x_apply(&view, eye)));
    TEST_ASSERT_TRUE(vec3x_equal((vec3x_t){MATHX_ONE, 0, 0}, mat4x_apply(&view, vec3x_add(eye, x))));
    TEST_ASSERT_TRUE(vec3x_equal((vec3x_t){0, MATHX_ONE, 0}, mat4x_apply(&view, vec3x_add(eye, y))));
    TEST_ASSERT_TRUE(vec3x_equal((vec3x_t){0, 0, MATHX_ONE}, mat4x_apply(&view, vec3x_add(eye, z))));
}

static void
test_fixed_scale_rows_preserves_the_bottom_row(void) {
    const vec3x_t scale = {2 * MATHX_ONE, -3 * MATHX_ONE, MATHX_ONE / 2};
    const mat4x_t m = {{{MATHX_ONE, -MATHX_ONE, MATHX_ONE / 2, 2 * MATHX_ONE},
                        {0, MATHX_ONE, -MATHX_ONE, 3 * MATHX_ONE},
                        {-MATHX_ONE, 0, MATHX_ONE, -2 * MATHX_ONE},
                        {MATHX_ONE, -MATHX_ONE, MATHX_ONE / 2, 0}}};
    const mat4x_t got = mat4x_scale_rows(m, scale);
    const int32_t factors[] = {scale.x, scale.y, scale.z, MATHX_ONE};
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            TEST_ASSERT_EQUAL_INT32(mathx_mul(m.m[r][c], factors[r]), got.m[r][c]);
        }
    }
}

static void
test_fixed_scale_columns_keeps_the_translation(void) {
    const vec3x_t scale = {2 * MATHX_ONE, -3 * MATHX_ONE, MATHX_ONE / 2};
    const mat4x_t m = {{{MATHX_ONE, -MATHX_ONE, MATHX_ONE / 2, 2 * MATHX_ONE},
                        {0, MATHX_ONE, -MATHX_ONE, 3 * MATHX_ONE},
                        {-MATHX_ONE, 0, MATHX_ONE, -2 * MATHX_ONE},
                        {0, 0, 0, MATHX_ONE}}};
    const mat4x_t got = mat4x_scale_columns(m, scale);
    const int32_t factors[] = {scale.x, scale.y, scale.z, MATHX_ONE};
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            const int32_t want = r < 3 ? mathx_mul(m.m[r][c], factors[c]) : m.m[r][c];
            TEST_ASSERT_EQUAL_INT32(want, got.m[r][c]);
        }
    }
}

static void
test_fixed_transform_matrix_and_view_match_float(void) {
    transformf_t f = TRANSFORMF_IDENTITY;
    transformx_t x = TRANSFORMX_IDENTITY;
    transformf_set_position(&f, (vec3f_t){1.0F, -2.0F, 3.0F});
    transformx_set_position(&x, to_x(1.0F, -2.0F, 3.0F));
    transformf_set_scale(&f, (vec3f_t){2.0F, 1.0F, 0.5F});
    transformx_set_scale(&x, to_x(2.0F, 1.0F, 0.5F));
    transformf_set_rotation(&f, quatf_from_euler((vec3f_t){0.4F, -0.3F, 0.2F}));
    transformx_set_rotation(&x,
                            quatx_from_euler((vec3x_t){(int32_t)(0.4F / MATH_TAU * Q), (int32_t)(-0.3F / MATH_TAU * Q),
                                                       (int32_t)(0.2F / MATH_TAU * Q)}));

    const vec3f_t p = {0.5F, 1.5F, -1.0F};
    const mat4f_t mf = transformf_matrix(&f);
    const mat4x_t mx = transformx_matrix(&x);
    const vec3x_t px = vec3x_from_vec3f(p);
    assert_x_near_f(mat4f_apply(&mf, p), mat4x_apply(&mx, px), FIXED_SLACK);

    const mat4f_t vf = transformf_view(&f);
    const mat4x_t vx = transformx_view(&x);
    assert_x_near_f(mat4f_apply(&vf, p), mat4x_apply(&vx, px), FIXED_SLACK);
}

static void
test_fixed_transform_look_at_matches_float(void) {
    transformf_t f = TRANSFORMF_IDENTITY;
    transformx_t x = TRANSFORMX_IDENTITY;
    transformf_set_position(&f, (vec3f_t){1.0F, -2.0F, 3.0F});
    transformx_set_position(&x, to_x(1.0F, -2.0F, 3.0F));
    transformf_look_at(&f, (vec3f_t){4.0F, 1.0F, 9.0F}, (vec3f_t){0.0F, 1.0F, 0.0F});
    transformx_look_at(&x, to_x(4.0F, 1.0F, 9.0F), to_x(0.0F, 1.0F, 0.0F));
    const vec3f_t fwd = {0.0F, 0.0F, 1.0F};
    assert_x_near_f(quatf_rotate(f.rotation, fwd), quatx_rotate(x.rotation, vec3x_from_vec3f(fwd)), FIXED_SLACK);
}

static bool
rebuilds_x(transformx_t* t) {
    t->matrix.m[0][3] = 12345;
    return transformx_matrix(t).m[0][3] != 12345;
}

static void
test_fixed_transform_builds_once_and_rebuilds_after_each_setter(void) {
    transformx_t t = TRANSFORMX_IDENTITY;
    TEST_ASSERT_FALSE(rebuilds_x(&t));
    ASSERT_TRANSFORM_SETTERS(transformx, t, to_x, quatx_from_axis_angle(to_x(0.0F, 1.0F, 0.0F), MATHX_ONE / 4),
                             rebuilds_x);

    transformx_t zero = {0};
    TEST_ASSERT_FALSE(zero.cached);
    (void)transformx_matrix(&zero);
    TEST_ASSERT_TRUE(zero.cached);
}

static void
test_round_trip_conversions_return_the_original_where_representable(void) {
    const vec3f_t v = {1.5F, -2.25F, 100.0F};
    TEST_ASSERT_EQUAL_FLOAT(v.y, vec3f_from_vec3x(vec3x_from_vec3f(v)).y);
    TEST_ASSERT_EQUAL_FLOAT(v.z, vec3f_from_vec3s(vec3s_from_vec3f(v, 0.25F), 0.25F).z);
    TEST_ASSERT_EQUAL_FLOAT(v.y, vec3f_from_vec3s(vec3s_from_vec3f(v, 0.25F), 0.25F).y);

    const vec3i_t i = {7, -9, 1000};
    TEST_ASSERT_TRUE(vec3i_equal(i, vec3i_from_vec3f(vec3f_from_vec3i(i))));
    TEST_ASSERT_TRUE(vec3i_equal(i, vec3i_from_vec3s(vec3s_from_vec3i(i))));
    const vec2i_t p = {-4, 600};
    TEST_ASSERT_TRUE(vec2i_equal(p, vec2i_from_vec2s(vec2s_from_vec2i(p))));
    TEST_ASSERT_EQUAL_INT(3, vec2i_from_vec2f((vec2f_t){2.6F, 0.0F}).x);
    TEST_ASSERT_EQUAL_FLOAT(0.5F, vec2f_from_vec2x(vec2x_from_vec2f((vec2f_t){0.5F, 0.0F})).x);
    TEST_ASSERT_EQUAL_FLOAT(8.0F, vec2f_from_vec2s(vec2s_from_vec2f((vec2f_t){8.0F, 0.0F}, 2.0F), 2.0F).x);
}

static void
test_fixed_point_overflow_saturates_and_divide_by_zero_follows_the_sign(void) {
    TEST_ASSERT_EQUAL_INT32(INT32_MAX, mathx_add(INT32_MAX, 1));
    TEST_ASSERT_EQUAL_INT32(INT32_MIN, mathx_sub(INT32_MIN, 1));
    TEST_ASSERT_EQUAL_INT32(INT32_MAX, mathx_neg(INT32_MIN));
    TEST_ASSERT_EQUAL_INT32(INT32_MAX, mathx_mul(INT32_MAX, INT32_MAX));
    TEST_ASSERT_EQUAL_INT32(INT32_MIN, mathx_mul(INT32_MAX, INT32_MIN));
    TEST_ASSERT_EQUAL_INT32(INT32_MAX, mathx_div(MATHX_ONE, 0));
    TEST_ASSERT_EQUAL_INT32(INT32_MIN, mathx_div(-MATHX_ONE, 0));
    TEST_ASSERT_EQUAL_INT32(0, mathx_div(0, 0));
    TEST_ASSERT_EQUAL_INT32(-MATHX_ONE, fx_div_round(INT32_MAX, 1, MATHX_SHIFT));
    TEST_ASSERT_EQUAL_INT32(INT32_MAX, mathx_div(INT32_MAX, 1));
    TEST_ASSERT_EQUAL_INT32(3 * MATHX_ONE / 2, mathx_div(3 * MATHX_ONE, 2 * MATHX_ONE));
    TEST_ASSERT_EQUAL_INT32(3 * MATHX_ONE, mathx_sqrt(9 * MATHX_ONE));
    TEST_ASSERT_EQUAL_INT32(0, mathx_sqrt(-5));

    const vec3x_t big = {INT32_MAX, INT32_MIN, 0};
    const vec3x_t sum = vec3x_add(big, (vec3x_t){1, -1, 0});
    TEST_ASSERT_EQUAL_INT32(INT32_MAX, sum.x);
    TEST_ASSERT_EQUAL_INT32(INT32_MIN, sum.y);
    TEST_ASSERT_EQUAL_INT32(INT32_MAX, vec3x_scale((vec3x_t){MATHX_ONE * 30000, 0, 0}, MATHX_ONE * 30000).x);
    TEST_ASSERT_EQUAL_INT32(INT32_MAX, vec2x_from_vec2f((vec2f_t){1.0e9F, 0.0F}).x);
    TEST_ASSERT_EQUAL_INT32(INT32_MIN, vec2x_from_vec2f((vec2f_t){-1.0e9F, 0.0F}).x);
}

static void
test_maths_saturate_exhaustive(void) {
    enum { MARGIN = 4096 };

    for (int32_t value = INT16_MIN - MARGIN; value <= INT16_MAX + MARGIN; value++) {
        const int16_t expected = value > INT16_MAX ? INT16_MAX : value < INT16_MIN ? INT16_MIN : (int16_t)value;
        TEST_ASSERT_EQUAL_INT16(expected, maths_saturate(value));
    }
    TEST_ASSERT_EQUAL_INT16(INT16_MIN, maths_saturate(INT32_MIN));
    TEST_ASSERT_EQUAL_INT16(INT16_MAX, maths_saturate(INT32_MAX));
    printf("MATHERROR name=maths_saturate type=s max=0 unit=lsb method=sweep\n");
}

static void
test_int16_arithmetic_and_conversion_saturate_at_the_int16_range(void) {

    TEST_ASSERT_EQUAL_INT16(INT16_MAX, vec3s_add((vec3s_t){INT16_MAX, 0, 0}, (vec3s_t){1, 0, 0}).x);
    TEST_ASSERT_EQUAL_INT16(INT16_MIN, vec3s_sub((vec3s_t){INT16_MIN, 0, 0}, (vec3s_t){1, 0, 0}).x);
    TEST_ASSERT_EQUAL_INT16(INT16_MAX, vec3s_scale((vec3s_t){300, 0, 0}, 300).x);
    TEST_ASSERT_EQUAL_INT16(INT16_MAX, vec3s_from_vec3i((vec3i_t){100000, 0, 0}).x);
    TEST_ASSERT_EQUAL_INT16(INT16_MIN, vec3s_from_vec3i((vec3i_t){0, -100000, 0}).y);
    TEST_ASSERT_EQUAL_INT16(INT16_MAX, vec3s_from_vec3f((vec3f_t){0.0F, 0.0F, 1.0e9F}, 1.0F).z);
    TEST_ASSERT_EQUAL_INT16(INT16_MAX, vec2s_from_vec2f((vec2f_t){50000.0F, 0.0F}, 1.0F).x);
    TEST_ASSERT_EQUAL_INT16(32767, vec3s_from_vec3f((vec3f_t){3276.7F, 0.0F, 0.0F}, 0.1F).x);
    TEST_ASSERT_TRUE(vec3s_dot((vec3s_t){INT16_MIN, INT16_MIN, INT16_MIN}, (vec3s_t){INT16_MIN, INT16_MIN, INT16_MIN})
                     == 3 * (int64_t)INT16_MIN * INT16_MIN);
}

static void
test_int32_dot_widens_to_int64(void) {
    const vec3i_t big = {100000, 100000, 100000};
    TEST_ASSERT_TRUE(vec3i_dot(big, big) == 30000000000LL);
}

static void
test_fixed_multiply_and_divide_round_ties_away_from_zero_in_both_signs(void) {
    TEST_ASSERT_EQUAL_INT32(2, mathx_mul(3, MATHX_ONE / 2)); /* 1.5 units */
    TEST_ASSERT_EQUAL_INT32(-2, mathx_mul(-3, MATHX_ONE / 2));
    TEST_ASSERT_EQUAL_INT32(1, mathx_mul(1, MATHX_ONE / 2)); /* 0.5 unit */
    TEST_ASSERT_EQUAL_INT32(-1, mathx_mul(-1, MATHX_ONE / 2));
    TEST_ASSERT_EQUAL_INT32(1, mathx_div(1, 2 * MATHX_ONE)); /* 0.5 unit */
    TEST_ASSERT_EQUAL_INT32(-1, mathx_div(-1, 2 * MATHX_ONE));
    TEST_ASSERT_EQUAL_INT32(-1, mathx_div(1, -2 * MATHX_ONE));
    TEST_ASSERT_EQUAL_INT32(0, mathx_div(1, 4 * MATHX_ONE)); /* 0.25 unit rounds down */
}

static void
test_fixed_divide_handles_signs_saturation_and_the_most_negative_divisor(void) {
    TEST_ASSERT_EQUAL_INT32(-3 * MATHX_ONE, mathx_div(-6 * MATHX_ONE, 2 * MATHX_ONE));
    TEST_ASSERT_EQUAL_INT32(-3 * MATHX_ONE, mathx_div(6 * MATHX_ONE, -2 * MATHX_ONE));
    TEST_ASSERT_EQUAL_INT32(3 * MATHX_ONE, mathx_div(-6 * MATHX_ONE, -2 * MATHX_ONE));
    TEST_ASSERT_EQUAL_INT32(INT32_MIN, mathx_div(INT32_MIN, MATHX_ONE / 2));
    TEST_ASSERT_EQUAL_INT32(INT32_MAX, mathx_div(INT32_MIN, -MATHX_ONE / 2));
    TEST_ASSERT_EQUAL_INT32(MATHX_ONE, mathx_div(INT32_MIN, INT32_MIN));
    TEST_ASSERT_EQUAL_INT32(-2, mathx_div(MATHX_ONE, INT32_MIN));
}

static void
test_fixed_square_root_floors_and_survives_the_largest_input(void) {
    TEST_ASSERT_EQUAL_INT32(92681, mathx_sqrt(2 * MATHX_ONE));
    const int32_t roots[] = {1, 3, 65535, 1000000, INT32_MAX};
    for (unsigned i = 0; i < sizeof(roots) / sizeof(roots[0]); i++) {
        const uint64_t n = (uint64_t)roots[i] << MATHX_SHIFT;
        const uint64_t r = (uint64_t)mathx_sqrt(roots[i]);
        TEST_ASSERT_TRUE(r * r <= n);
        TEST_ASSERT_TRUE((r + 1) * (r + 1) > n);
    }
}

static void
test_int16_negative_edges_and_float_rounding_boundaries(void) {
    TEST_ASSERT_EQUAL_INT16(INT16_MAX, maths_neg(INT16_MIN));
    TEST_ASSERT_EQUAL_INT16(INT16_MIN, maths_mul(INT16_MIN, 2));
    TEST_ASSERT_EQUAL_INT16(INT16_MIN, maths_mul(-200, 200));
    TEST_ASSERT_EQUAL_INT16(INT16_MIN, vec3s_from_vec3f((vec3f_t){-1.0e9F, 0.0F, 0.0F}, 1.0F).x);
    TEST_ASSERT_EQUAL_INT32(3, mathf_round_i32(2.5F));
    TEST_ASSERT_EQUAL_INT32(-3, mathf_round_i32(-2.5F));
    TEST_ASSERT_EQUAL_INT32(INT32_MAX, mathf_round_i32(MATH_FLOAT_INT_LIMIT));
    TEST_ASSERT_EQUAL_INT32(INT32_MIN, mathf_round_i32(-MATH_FLOAT_INT_LIMIT));
    TEST_ASSERT_EQUAL_INT32(2147483392, mathf_round_i32(2147483392.0F));
}

static void
test_normalize_by_value_and_repeated_rotation_stay_unit_length(void) {
    const quatf_t f = quatf_normalize((quatf_t){0.0F, 0.0F, 0.0F, 2.0F});
    TEST_ASSERT_EQUAL_FLOAT(1.0F, f.w);
    const quatx_t x = quatx_normalize((quatx_t){0, 0, 0, 2 * MATHX_ONE});
    TEST_ASSERT_EQUAL_INT32(MATHX_ONE, x.w);

    transformf_t tf = TRANSFORMF_IDENTITY;
    transformx_t tx = TRANSFORMX_IDENTITY;
    for (int i = 0; i < 200; i++) {
        transformf_rotate(&tf, quatf_from_euler((vec3f_t){0.01F, 0.02F, 0.03F}));
        transformx_rotate(&tx, quatx_from_euler((vec3x_t){MATHX_ONE / 600, MATHX_ONE / 300, MATHX_ONE / 200}));
    }
    const quatf_t q = tf.rotation;
    TEST_ASSERT_FLOAT_WITHIN(1e-4F, 1.0F, (q.x * q.x) + (q.y * q.y) + (q.z * q.z) + (q.w * q.w));
    const float w = mathx_to_f(tx.rotation.w);
    const float lengthsq = mathx_to_f(mathx_mul(tx.rotation.x, tx.rotation.x))
                           + mathx_to_f(mathx_mul(tx.rotation.y, tx.rotation.y))
                           + mathx_to_f(mathx_mul(tx.rotation.z, tx.rotation.z)) + (w * w);
    TEST_ASSERT_FLOAT_WITHIN(5e-3F, 1.0F, lengthsq);
}

static void
test_slerp_of_equal_and_opposite_quaternions_and_across_its_threshold(void) {
    const quatf_t a = quatf_from_axis_angle((vec3f_t){0.0F, 1.0F, 0.0F}, 0.8F);
    const quatf_t same = quatf_slerp(a, a, 0.3F);
    TEST_ASSERT_FLOAT_WITHIN(1e-5F, 1.0F, fabsf((same.x * a.x) + (same.y * a.y) + (same.z * a.z) + (same.w * a.w)));

    const quatf_t opposite = {-a.x, -a.y, -a.z, -a.w};
    const quatf_t mid = quatf_slerp(a, opposite, 0.5F);
    TEST_ASSERT_FLOAT_WITHIN(1e-5F, 1.0F, fabsf((mid.x * a.x) + (mid.y * a.y) + (mid.z * a.z) + (mid.w * a.w)));

    /* cos(half angle) of 0.9996 and 0.9994 sit either side of the 0.9995
     * switch to a normalized lerp; both must land on half the angle. */
    const float cosines[] = {0.9996F, 0.9994F};
    for (int i = 0; i < 2; i++) {
        const float angle = 2.0F * acosf(cosines[i]);
        const quatf_t end = quatf_from_axis_angle((vec3f_t){0.0F, 1.0F, 0.0F}, angle);
        const vec3f_t got = quatf_rotate(quatf_slerp(quatf_identity(), end, 0.5F), (vec3f_t){0.0F, 0.0F, 1.0F});
        TEST_ASSERT_FLOAT_WITHIN(1e-4F, sinf(angle / 2.0F), got.x);
        TEST_ASSERT_FLOAT_WITHIN(1e-4F, cosf(angle / 2.0F), got.z);
    }
}

static void
assert_basis_f(vec3f_t r, vec3f_t u, vec3f_t f) {
    const quatf_t q = quatf_from_basis(r, u, f);
    const vec3f_t x = quatf_rotate(q, (vec3f_t){1.0F, 0.0F, 0.0F});
    const vec3f_t y = quatf_rotate(q, (vec3f_t){0.0F, 1.0F, 0.0F});
    const vec3f_t z = quatf_rotate(q, (vec3f_t){0.0F, 0.0F, 1.0F});
    TEST_ASSERT_FLOAT_WITHIN(1e-5F, r.x, x.x);
    TEST_ASSERT_FLOAT_WITHIN(1e-5F, u.y, y.y);
    TEST_ASSERT_FLOAT_WITHIN(1e-5F, f.z, z.z);
    TEST_ASSERT_FLOAT_WITHIN(1e-5F, r.z, x.z);
    TEST_ASSERT_FLOAT_WITHIN(1e-5F, f.x, z.x);
}

static void
assert_basis_x(vec3f_t r, vec3f_t u, vec3f_t f) {
    const quatx_t q = quatx_from_basis(vec3x_from_vec3f(r), vec3x_from_vec3f(u), vec3x_from_vec3f(f));
    assert_x_near_f(r, quatx_rotate(q, to_x(1.0F, 0.0F, 0.0F)), FIXED_SLACK);
    assert_x_near_f(u, quatx_rotate(q, to_x(0.0F, 1.0F, 0.0F)), FIXED_SLACK);
    assert_x_near_f(f, quatx_rotate(q, to_x(0.0F, 0.0F, 1.0F)), FIXED_SLACK);
}

/* A half turn about the unit axis n: R = 2 n n^T - I, whose diagonal says which branch of
 * from_basis it takes; an off-axis n keeps the other two diagonal entries apart. */
static void
check_half_turn_about(vec3f_t n) {
    const vec3f_t r = {(2.0F * n.x * n.x) - 1.0F, 2.0F * n.y * n.x, 2.0F * n.z * n.x};
    const vec3f_t u = {2.0F * n.x * n.y, (2.0F * n.y * n.y) - 1.0F, 2.0F * n.z * n.y};
    const vec3f_t f = {2.0F * n.x * n.z, 2.0F * n.y * n.z, (2.0F * n.z * n.z) - 1.0F};
    assert_basis_f(r, u, f);
    assert_basis_x(r, u, f);
}

static void
test_from_basis_picks_the_largest_diagonal_for_off_axis_half_turns(void) {
    check_half_turn_about((vec3f_t){0.9486833F, 0.1F, 0.3F});
    check_half_turn_about((vec3f_t){0.1F, 0.9486833F, 0.3F});
    check_half_turn_about((vec3f_t){0.3F, 0.1F, 0.9486833F});
}

static void
test_fixed_angles_negative_and_beyond_a_turn(void) {
    const vec3x_t y_axis = to_x(0.0F, 1.0F, 0.0F);
    const vec3x_t z = to_x(0.0F, 0.0F, 1.0F);
    assert_x_near_f((vec3f_t){-1.0F, 0.0F, 0.0F}, quatx_rotate(quatx_from_axis_angle(y_axis, -MATHX_ONE / 4), z),
                    FIXED_SLACK);
    assert_x_near_f((vec3f_t){1.0F, 0.0F, 0.0F},
                    quatx_rotate(quatx_from_axis_angle(y_axis, MATHX_ONE + (MATHX_ONE / 4)), z), FIXED_SLACK);
    assert_x_near_f((vec3f_t){1.0F, 0.0F, 0.0F},
                    quatx_rotate(quatx_from_axis_angle(y_axis, -3 * MATHX_ONE + (MATHX_ONE / 4)), z), FIXED_SLACK);
}

static void
test_fixed_dot_fast_paths_floor_each_term_and_add_the_constant(void) {
    const int32_t half = MATHX_ONE / 2;
    /* A half unit of Q16.16 per term floors to zero, and a negative one to -1. */
    TEST_ASSERT_EQUAL_INT32(7, mathx_dot3c(1, half, 1, half, 1, half, 7));
    TEST_ASSERT_EQUAL_INT32(4, mathx_dot3c(-1, half, -1, half, -1, half, 7));
    TEST_ASSERT_EQUAL_INT32(5 * MATHX_ONE, mathx_dot2c(2 * MATHX_ONE, MATHX_ONE, MATHX_ONE, 3 * MATHX_ONE, 0));
    TEST_ASSERT_EQUAL_INT32(MATHX_ONE + 9, mathx_dot2c(MATHX_ONE, MATHX_ONE, 0, 0, 9));
    /* The sum wraps rather than saturating: its contract is bounded coordinates. */
    TEST_ASSERT_EQUAL_INT32(INT32_MIN, mathx_dot2c(0, 0, 0, 0, INT32_MIN));
}

static void
test_the_narrow_dot_is_a_plain_32_bit_sum_then_a_floor_shift(void) {
    TEST_ASSERT_EQUAL_INT32((12 + 30 + 56 + 100) >> 2, mathx_dot3_narrow(3, 4, 5, 6, 7, 8, 100, 2));
    TEST_ASSERT_EQUAL_INT32(-1, mathx_dot3_narrow(-1, 1, 0, 0, 0, 0, 0, 3)); /* floors toward -infinity */
    TEST_ASSERT_EQUAL_INT32(1 << 29, mathx_dot3_narrow(1 << 14, 1 << 15, 0, 0, 0, 0, 0, 0));
}

static void
test_fixed_matrix_apply_matches_float(void) {
    const mat4f_t mf = mat4f_from_trs((vec3f_t){1.0F, -2.0F, 3.0F}, quatf_from_euler((vec3f_t){0.4F, -0.3F, 0.2F}),
                                      (vec3f_t){2.0F, 1.0F, 0.5F});
    mat4x_t mx;
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            mx.m[r][c] = mathf_to_x(mf.m[r][c]);
        }
    }
    const vec3f_t p = {0.5F, 1.5F, -1.0F};
    assert_x_near_f(mat4f_apply(&mf, p), mat4x_apply(&mx, vec3x_from_vec3f(p)), 1e-3F);
}

static void
test_fixed_affine_product_applies_the_right_matrix_first(void) {
    const mat4x_t mx = mat4x_from_trs((vec3x_t){MATHX_ONE, -2 * MATHX_ONE, 3 * MATHX_ONE}, quatx_identity(),
                                      (vec3x_t){2 * MATHX_ONE, MATHX_ONE, MATHX_ONE / 2});
    const vec3f_t p = {0.5F, 1.5F, -1.0F};
    const mat4x_t other = mat4x_from_trs((vec3x_t){2 * MATHX_ONE, MATHX_ONE, -MATHX_ONE}, quatx_identity(),
                                         (vec3x_t){MATHX_ONE, 2 * MATHX_ONE, MATHX_ONE});
    const mat4x_t full = mat4x_mul(mx, other), affine = mat4x_mul_affine(mx, other);
    assert_x_near_f(vec3f_from_vec3x(mat4x_apply(&mx, mat4x_apply(&other, vec3x_from_vec3f(p)))),
                    mat4x_apply(&affine, vec3x_from_vec3f(p)), 1e-3F);
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            TEST_ASSERT_EQUAL_INT32(full.m[r][c], affine.m[r][c]);
        }
    }
}

/* A swizzle family as rows of name and function, built by the template's own
 * iterator. The source holds 10, 20 and 30, so the letters in a name alone say
 * what the result holds; the row count catches an iterator that skips one. */
#define SWIZZLE_ROW2(D, S, a, b)    {#a #b, S##_##a##b},
#define SWIZZLE_ROW3(D, S, a, b, c) {#a #b #c, S##_##a##b##c},
#define SWIZZLE_GOT2(r)             {(int)(r).x, (int)(r).y}
#define SWIZZLE_GOT3(r)             {(int)(r).x, (int)(r).y, (int)(r).z}

#define SWIZZLE_CHECK(D, S, N, K, ...)                                                                                 \
    static void check_##S##_to_##D(void) {                                                                             \
        const S##_t v = {__VA_ARGS__};                                                                                 \
        const struct {                                                                                                 \
            const char* name;                                                                                          \
            D##_t (*fn)(S##_t);                                                                                        \
        } rows[] = {MATH_SWIZZLE_EACH##K(N, SWIZZLE_ROW##K, D, S)};                                                    \
        const int count = (int)(sizeof rows / sizeof rows[0]);                                                         \
        TEST_ASSERT_EQUAL_INT(K == 2 ? N * N : N * N * N, count);                                                      \
        for (int i = 0; i < count; i++) {                                                                              \
            const D##_t r = rows[i].fn(v);                                                                             \
            const int got[] = SWIZZLE_GOT##K(r);                                                                       \
            for (int c = 0; c < K; c++) {                                                                              \
                TEST_ASSERT_EQUAL_INT_MESSAGE(10 * (rows[i].name[c] - 'w'), got[c], rows[i].name);                     \
            }                                                                                                          \
            for (int j = 0; j < i; j++) {                                                                              \
                TEST_ASSERT_NOT_EQUAL_INT_MESSAGE(0, strcmp(rows[i].name, rows[j].name), rows[i].name);                \
            }                                                                                                          \
        }                                                                                                              \
    }

SWIZZLE_CHECK(vec2f, vec2f, 2, 2, 10.0F, 20.0F)
SWIZZLE_CHECK(vec2i, vec2i, 2, 2, 10, 20)
SWIZZLE_CHECK(vec2s, vec2s, 2, 2, 10, 20)
SWIZZLE_CHECK(vec2x, vec2x, 2, 2, 10, 20)
SWIZZLE_CHECK(vec3f, vec3f, 3, 3, 10.0F, 20.0F, 30.0F)
SWIZZLE_CHECK(vec3i, vec3i, 3, 3, 10, 20, 30)
SWIZZLE_CHECK(vec3s, vec3s, 3, 3, 10, 20, 30)
SWIZZLE_CHECK(vec3x, vec3x, 3, 3, 10, 20, 30)
SWIZZLE_CHECK(vec2f, vec3f, 3, 2, 10.0F, 20.0F, 30.0F)
SWIZZLE_CHECK(vec2i, vec3i, 3, 2, 10, 20, 30)
SWIZZLE_CHECK(vec2s, vec3s, 3, 2, 10, 20, 30)
SWIZZLE_CHECK(vec2x, vec3x, 3, 2, 10, 20, 30)
SWIZZLE_CHECK(vec3f, vec2f, 2, 3, 10.0F, 20.0F)
SWIZZLE_CHECK(vec3i, vec2i, 2, 3, 10, 20)
SWIZZLE_CHECK(vec3s, vec2s, 2, 3, 10, 20)
SWIZZLE_CHECK(vec3x, vec2x, 2, 3, 10, 20)

static void
test_every_swizzle_family_moves_the_named_components_for_every_number_type(void) {
    void (*const families[])(void) = {
        check_vec2f_to_vec2f, check_vec2i_to_vec2i, check_vec2s_to_vec2s, check_vec2x_to_vec2x,
        check_vec3f_to_vec3f, check_vec3i_to_vec3i, check_vec3s_to_vec3s, check_vec3x_to_vec3x,
        check_vec3f_to_vec2f, check_vec3i_to_vec2i, check_vec3s_to_vec2s, check_vec3x_to_vec2x,
        check_vec2f_to_vec3f, check_vec2i_to_vec3i, check_vec2s_to_vec3s, check_vec2x_to_vec3x,
    };
    for (size_t i = 0; i < sizeof families / sizeof families[0]; i++) {
        families[i]();
    }
}

static void
test_a_cross_dimension_swizzle_returns_the_other_vector_type(void) {
    const vec3f_t p = {1.5F, -2.0F, 4.0F};
    const vec2f_t ground = vec3f_xz(p);
    TEST_ASSERT_TRUE(_Generic(vec3f_xz(p), vec2f_t: true, default: false));
    TEST_ASSERT_TRUE(_Generic(vec2x_yxy((vec2x_t){0, 0}), vec3x_t: true, default: false));
    TEST_ASSERT_TRUE(vec2f_equal((vec2f_t){1.5F, 4.0F}, ground));
    TEST_ASSERT_TRUE(vec3s_equal((vec3s_t){-7, 3, -7}, vec2s_yxy((vec2s_t){3, -7})));
}

static void
test_a_vec3_from_a_vec2_and_z_keeps_every_component_for_every_number_type(void) {
    const struct {
        const char* type;
        bool built;
    } rows[] = {
        {"f", vec3f_equal((vec3f_t){1.5F, -2.0F, 3.0F}, vec3f_from_xy((vec2f_t){1.5F, -2.0F}, 3.0F))},
        {"i", vec3i_equal((vec3i_t){INT32_MIN, -2, INT32_MAX}, vec3i_from_xy((vec2i_t){INT32_MIN, -2}, INT32_MAX))},
        {"s", vec3s_equal((vec3s_t){INT16_MIN, -2, INT16_MAX}, vec3s_from_xy((vec2s_t){INT16_MIN, -2}, INT16_MAX))},
        {"x",
         vec3x_equal(to_x(1.5F, -2.0F, 3.0F), vec3x_from_xy(vec2x_from_vec2f((vec2f_t){1.5F, -2.0F}), 3 * MATHX_ONE))},
        {"f round trip",
         vec3f_equal((vec3f_t){1.0F, 2.0F, 3.0F}, vec3f_from_xy(vec3f_xy((vec3f_t){1.0F, 2.0F, 3.0F}), 3.0F))},
    };

    for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        TEST_ASSERT_TRUE_MESSAGE(rows[i].built, rows[i].type);
    }
}

static uint32_t
float_bits(float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof bits);
    return bits;
}

static float
float_from_bits(uint32_t bits) {
    float value;
    memcpy(&value, &bits, sizeof value);
    return value;
}

#define RECIP_OCTAVES           20
#define FAST_PATH_RECIP_OCTAVES 1
#define FAST_PATH_BINS_PER_UNIT 32
#if !defined(ESP_PLATFORM) || (defined(__XTENSA__) && CONFIG_MATH_FPU_RECIP)
#define FAST_PATH_ROUNDING_ULP  0.5
#define FAST_PATH_RSQRT_OCTAVES 2
#define FAST_PATH_RSQRT_BINS    (((1 << FAST_PATH_RSQRT_OCTAVES) - 1) * FAST_PATH_BINS_PER_UNIT)
#endif

static float
fast_path_period_position(float input, int octaves) {
    int exponent;
    const float fraction = frexpf(fabsf(input), &exponent);
    const int octave = ((exponent - 1) % octaves + octaves) % octaves;
    return ldexpf(fraction * 2.0F, octave);
}

static unsigned
fast_path_period_bin(float input, int octaves) {
    const float position = fast_path_period_position(input, octaves);
    const unsigned bin = (unsigned)((position - 1.0F) * FAST_PATH_BINS_PER_UNIT);
    return bin;
}

static inline __attribute__((always_inline)) double
fast_path_fractional_ulp(float got, double exact) {
    int exponent;
    (void)frexp(fabs(exact), &exponent);
    const int shift = exponent - FLT_MANT_DIG;
    const double ulp = ldexp(1.0, shift < FLT_MIN_EXP - FLT_MANT_DIG ? FLT_MIN_EXP - FLT_MANT_DIG : shift);
    return fabs((double)got - exact) / ulp;
}

#ifndef ESP_PLATFORM
static void
test_fast_path_period_bins_and_fractional_ulp(void) {
    double bins[FAST_PATH_BINS_PER_UNIT] = {0};
    for (int exponent = -RECIP_OCTAVES; exponent <= RECIP_OCTAVES; exponent++) {
        const float input = ldexpf(1.5F, exponent);
        TEST_ASSERT_EQUAL_FLOAT(1.5F, fast_path_period_position(input, FAST_PATH_RECIP_OCTAVES));
        TEST_ASSERT_EQUAL_FLOAT(exponent % FAST_PATH_RSQRT_OCTAVES == 0 ? 1.5F : 3.0F,
                                fast_path_period_position(-input, FAST_PATH_RSQRT_OCTAVES));
        const unsigned bin = fast_path_period_bin(input, FAST_PATH_RECIP_OCTAVES);
        bins[bin] = fmax(bins[bin], exponent + RECIP_OCTAVES);
    }
    TEST_ASSERT_TRUE(bins[FAST_PATH_BINS_PER_UNIT / 2] == 2 * RECIP_OCTAVES);
    const double exact = 1.0 / 3.0;
    TEST_ASSERT_TRUE(fast_path_fractional_ulp((float)exact, exact) > 0.0);
    TEST_ASSERT_TRUE(fast_path_fractional_ulp((float)exact, exact) < FAST_PATH_ROUNDING_ULP);
    TEST_ASSERT_TRUE(fast_path_fractional_ulp(1.0F, 1.0) == 0.0);
    TEST_ASSERT_TRUE(fast_path_fractional_ulp(0.0F, (double)FLT_TRUE_MIN / 4) == 0.25);
    TEST_ASSERT_EQUAL_UINT(0, fast_path_period_bin(1.0F, FAST_PATH_RECIP_OCTAVES));
    TEST_ASSERT_EQUAL_UINT(FAST_PATH_RSQRT_BINS - 1,
                           fast_path_period_bin(nextafterf(4.0F, 0.0F), FAST_PATH_RSQRT_OCTAVES));
}

#endif

/* Within an ulp of the quotient, measured on the side the result landed:
 * both signs, a geometric sweep over 2^-20..2^20 and each octave's edges (a
 * power of two, its neighbours, the largest mantissa below it). On the
 * sweep it is the quotient nearly always; the edges round off it more. */
#define RECIP_SWEEP_STEP    1.0137F /* about 50 samples an octave */
#define RECIP_EXACT_PERCENT 99

/* Asserts mathf_recip(x) within an ulp of 1 / x; true when it is exactly. */
static inline __attribute__((always_inline)) bool
reciprocal_is_exact(float x, uint32_t* maximum, double* bins) {
    const float want = 1.0F / x;
    const float got = mathf_recip(x);
    const float ulp = fabsf(nextafterf(want, got) - want);
    TEST_ASSERT_FLOAT_WITHIN(MATHF_RECIP_ULP_LIMIT * ulp, want, got);
    const uint32_t a = float_bits(want);
    const uint32_t b = float_bits(got);
    const uint32_t error = a > b ? a - b : b - a;
    if (error > *maximum) {
        *maximum = error;
    }
    const unsigned bin = fast_path_period_bin(x, FAST_PATH_RECIP_OCTAVES);
    bins[bin] = fmax(bins[bin], fast_path_fractional_ulp(got, 1.0 / (double)x));
    return got == want;
}

static void
test_the_reciprocal_is_within_an_ulp_of_the_quotient_and_usually_on_it(void) {
    int count = 0;
    int exact = 0;
    uint32_t maximum = 0;
    double bins[FAST_PATH_BINS_PER_UNIT] = {0};
    for (int exponent = -RECIP_OCTAVES; exponent <= RECIP_OCTAVES; exponent++) {
        uint32_t bin_maximum = 0;
        for (int sign = -1; sign <= 1; sign += 2) {
            const float low = ldexpf(1.0F, exponent);
            if (exponent < RECIP_OCTAVES) {
                for (float a = low; a < 2.0F * low; a *= RECIP_SWEEP_STEP) {
                    exact += reciprocal_is_exact((float)sign * a, &bin_maximum, bins);
                    count++;
                }
            }
            const float power = (float)sign * low;
            (void)reciprocal_is_exact(power, &bin_maximum, bins);
            (void)reciprocal_is_exact(nextafterf(power, 0.0F), &bin_maximum, bins);
            (void)reciprocal_is_exact(nextafterf(power, 2.0F * power), &bin_maximum, bins);
            (void)reciprocal_is_exact(nextafterf(2.0F * power, 0.0F), &bin_maximum, bins);
        }
        if (bin_maximum > maximum) {
            maximum = bin_maximum;
        }
    }
    for (unsigned bin = 0; bin < FAST_PATH_BINS_PER_UNIT; bin++) {
        printf("MATHERRORBIN name=mathf_recip type=f x=%g max=%.12g unit=ulp axis=period\n",
               1.0 + (bin + 0.5) / FAST_PATH_BINS_PER_UNIT, bins[bin]);
    }
    TEST_ASSERT_GREATER_OR_EQUAL_INT(count * RECIP_EXACT_PERCENT / 100, exact);
    printf("MATHERROR name=mathf_recip type=f max=%lu unit=ulp method=sweep\n", (unsigned long)maximum);
}

static void
test_integer_repeat_and_wrap_cover_negative_values_and_int_limits(void) {
    const int length = 7;
    const int lo = -10;
    const int hi = -3;
    const int negative = -9;
    TEST_ASSERT_EQUAL_INT(length - 2, mathi_repeat(negative, length));
    TEST_ASSERT_EQUAL_INT(0, mathi_repeat(-length, length));
    TEST_ASSERT_EQUAL_INT(0, mathi_repeat(length, length));
    TEST_ASSERT_EQUAL_INT(0, mathi_repeat(0, length));
    TEST_ASSERT_EQUAL_INT((int)(((int64_t)INT_MIN % length + length) % length), mathi_repeat(INT_MIN, length));
    TEST_ASSERT_EQUAL_INT(INT_MAX % length, mathi_repeat(INT_MAX, length));
    TEST_ASSERT_EQUAL_INT(lo, mathi_wrap(lo, lo, hi));
    TEST_ASSERT_EQUAL_INT(lo, mathi_wrap(hi, lo, hi));
    TEST_ASSERT_EQUAL_INT(negative, mathi_wrap(negative, lo, hi));
    TEST_ASSERT_EQUAL_INT(hi - 1, mathi_wrap(lo - 1, lo, hi));
    const int several_widths = 4;
    TEST_ASSERT_EQUAL_INT(lo, mathi_wrap(lo - length, lo, hi));
    TEST_ASSERT_EQUAL_INT(lo, mathi_wrap(lo - several_widths * length, lo, hi));
    const int crossing_lo = -3;
    const int crossing_hi = 5;
    const int crossing_width = crossing_hi - crossing_lo;
    TEST_ASSERT_EQUAL_INT(crossing_lo, mathi_wrap(crossing_lo - crossing_width, crossing_lo, crossing_hi));
    TEST_ASSERT_EQUAL_INT(crossing_lo,
                          mathi_wrap(crossing_lo - several_widths * crossing_width, crossing_lo, crossing_hi));
    const int extremes[] = {INT_MIN, INT_MAX};
    const int below_negative_range = -20;
    const int above_negative_range = 8;
    const int negative_range_expected = -6;
    TEST_ASSERT_EQUAL_INT(negative_range_expected, mathi_wrap(below_negative_range, lo, hi));
    TEST_ASSERT_EQUAL_INT(negative_range_expected, mathi_wrap(above_negative_range, lo, hi));
    const int bounds[][2] = {{lo, hi}, {INT_MIN, INT_MIN + INT_MAX}, {0, INT_MAX}, {INT_MIN, INT_MAX}};
    for (size_t r = 0; r < sizeof bounds / sizeof bounds[0]; r++) {
        const int64_t width = (int64_t)bounds[r][1] - bounds[r][0];
        for (size_t i = 0; i < sizeof extremes / sizeof extremes[0]; i++) {
            const int64_t offset = (int64_t)extremes[i] - bounds[r][0];
            const int want = (int)(bounds[r][0] + (offset % width + width) % width);
            TEST_ASSERT_EQUAL_INT(want, mathi_wrap(extremes[i], bounds[r][0], bounds[r][1]));
        }
    }
    const int full_width_endpoint_expected = INT_MIN;
    const int full_width_negative_expected = -1;
    const int full_width_zero_expected = 0;
    TEST_ASSERT_EQUAL_INT(full_width_endpoint_expected, mathi_wrap(INT_MAX, INT_MIN, INT_MAX));
    TEST_ASSERT_EQUAL_INT(full_width_negative_expected, mathi_wrap(-1, INT_MIN, INT_MAX));
    TEST_ASSERT_EQUAL_INT(full_width_zero_expected, mathi_wrap(0, INT_MIN, INT_MAX));
}

static void
test_unsigned_repeat_keeps_clock_precision_and_clamp_covers_the_range(void) {
    const uint32_t duration = MATH_MS_PER_S;
    const uint32_t clock = UINT32_MAX - 1u;
    const uint32_t lo = UINT32_MAX - duration;
    const uint32_t hi = UINT32_MAX - 1u;
    TEST_ASSERT_EQUAL_UINT32(clock % duration, mathi_repeat_u32(clock, duration));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX % duration, mathi_repeat_u32(UINT32_MAX, duration));
    TEST_ASSERT_EQUAL_UINT32(0, mathi_repeat_u32(duration, duration));
    TEST_ASSERT_EQUAL_UINT32(0, mathi_repeat_u32(0, duration));
    TEST_ASSERT_EQUAL_UINT32(lo, mathi_clamp_u32(lo - 1u, lo, hi));
    TEST_ASSERT_EQUAL_UINT32(lo, mathi_clamp_u32(lo, lo, hi));
    TEST_ASSERT_EQUAL_UINT32(lo + 1u, mathi_clamp_u32(lo + 1u, lo, hi));
    TEST_ASSERT_EQUAL_UINT32(hi, mathi_clamp_u32(hi, lo, hi));
    TEST_ASSERT_EQUAL_UINT32(hi, mathi_clamp_u32(UINT32_MAX, lo, hi));
    const uint32_t signed_boundary = (uint32_t)INT32_MAX + 1u;
    const uint32_t crossing_lo = signed_boundary - duration;
    const uint32_t crossing_hi = signed_boundary + duration;
    TEST_ASSERT_EQUAL_UINT32(crossing_lo, mathi_clamp_u32(0, crossing_lo, crossing_hi));
    TEST_ASSERT_EQUAL_UINT32(crossing_lo, mathi_clamp_u32(crossing_lo - 1u, crossing_lo, crossing_hi));
    TEST_ASSERT_EQUAL_UINT32(crossing_lo, mathi_clamp_u32(crossing_lo, crossing_lo, crossing_hi));
    TEST_ASSERT_EQUAL_UINT32(signed_boundary - 1u, mathi_clamp_u32(signed_boundary - 1u, crossing_lo, crossing_hi));
    TEST_ASSERT_EQUAL_UINT32(signed_boundary, mathi_clamp_u32(signed_boundary, crossing_lo, crossing_hi));
    TEST_ASSERT_EQUAL_UINT32(signed_boundary + 1u, mathi_clamp_u32(signed_boundary + 1u, crossing_lo, crossing_hi));
    TEST_ASSERT_EQUAL_UINT32(crossing_hi, mathi_clamp_u32(crossing_hi, crossing_lo, crossing_hi));
    TEST_ASSERT_EQUAL_UINT32(crossing_hi, mathi_clamp_u32(crossing_hi + 1u, crossing_lo, crossing_hi));
    TEST_ASSERT_EQUAL_UINT32(crossing_hi, mathi_clamp_u32(UINT32_MAX, crossing_lo, crossing_hi));
}

static void
test_scalar_clamp_maps_nonfinite_values_to_bounds(void) {
    const float lo = -2.0F;
    const float hi = 3.0F;
    TEST_ASSERT_EQUAL_FLOAT(lo, mathf_clamp_finite(NAN, lo, hi));
    TEST_ASSERT_EQUAL_FLOAT(hi, mathf_clamp_finite(INFINITY, lo, hi));
    TEST_ASSERT_EQUAL_FLOAT(lo, mathf_clamp_finite(-INFINITY, lo, hi));
    TEST_ASSERT_EQUAL_FLOAT(lo, mathf_clamp_finite(-FLT_MAX, lo, hi));
    TEST_ASSERT_EQUAL_FLOAT(hi, mathf_clamp_finite(FLT_MAX, lo, hi));
    TEST_ASSERT_EQUAL_FLOAT(lo, mathf_clamp_finite(lo, lo, hi));
    TEST_ASSERT_EQUAL_FLOAT(hi, mathf_clamp_finite(hi, lo, hi));
    TEST_ASSERT_EQUAL_FLOAT(0, mathf_clamp_finite(0, lo, hi));
    TEST_ASSERT_EQUAL_FLOAT(0.5F, mathf_clamp_finite(0.5F, lo, hi));
    TEST_ASSERT_EQUAL_FLOAT(-0.5F, mathf_clamp_finite(-0.5F, lo, hi));
}

static void
test_scalar_rounding_preserves_large_integers_and_half_away_ties(void) {
    static const float values[] = {0, 0.49F, -0.49F, 0.5F, -0.5F, 1.5F, -1.5F};
    static const int32_t rounded[] = {0, 0, 0, 1, -1, 2, -2};
    for (size_t i = 0; i < sizeof values / sizeof values[0]; i++) {
        TEST_ASSERT_EQUAL_INT32(rounded[i], mathf_round_i32(values[i]));
    }
    const int32_t odd = (1U << (FLT_MANT_DIG - 1)) + 1;
    TEST_ASSERT_EQUAL_INT32(odd, mathf_round_i32((float)odd));
    TEST_ASSERT_EQUAL_INT32(-odd, mathf_round_i32((float)-odd));
    TEST_ASSERT_EQUAL_INT32(INT32_MAX, mathf_round_i32(MATH_FLOAT_INT_LIMIT));
    TEST_ASSERT_EQUAL_INT32(INT32_MIN, mathf_round_i32(-MATH_FLOAT_INT_LIMIT));
}

static void
test_scalar_int16_rounding_saturates_and_preserves_half_neighbors(void) {
    static const float edges[] = {NAN, -INFINITY, INFINITY, -FLT_MAX, FLT_MAX, INT16_MIN, INT16_MAX, 0, -0.0F};
    static const int16_t expected[] = {INT16_MIN, INT16_MIN, INT16_MAX, INT16_MIN, INT16_MAX,
                                       INT16_MIN, INT16_MAX, 0,         0};
    for (size_t i = 0; i < sizeof edges / sizeof edges[0]; i++) {
        TEST_ASSERT_EQUAL_INT16(expected[i], mathf_round_i16(edges[i]));
    }
    for (int whole = INT16_MIN; whole < INT16_MAX; whole++) {
        const float half = (float)whole + 0.5F;
        const float neighbors[] = {nextafterf(half, -INFINITY), half, nextafterf(half, INFINITY)};
        for (size_t i = 0; i < sizeof neighbors / sizeof neighbors[0]; i++) {
            TEST_ASSERT_EQUAL_INT16((int16_t)roundf(neighbors[i]), mathf_round_i16(neighbors[i]));
        }
    }
}

static void
test_transform_matrix_refusals_and_shear_tolerance_boundary(void) {
    mat4f_t matrix = mat4f_identity();
    transformf_t pose;
    matrix.m[0][3] = INFINITY;
    TEST_ASSERT_FALSE(transformf_from_mat(matrix, &pose));
    matrix = mat4f_identity();
    matrix.m[3][3] = 2;
    TEST_ASSERT_FALSE(transformf_from_mat(matrix, &pose));
    matrix = mat4f_identity();
    matrix.m[0][1] = TRANSFORMF_MATRIX_TOLERANCE;
    TEST_ASSERT_TRUE(transformf_from_mat(matrix, &pose));
    matrix.m[0][1] = nextafterf(TRANSFORMF_MATRIX_TOLERANCE, INFINITY);
    TEST_ASSERT_FALSE(transformf_from_mat(matrix, &pose));
}

static void
test_unsigned_64_repeat_preserves_high_clock_bits_and_wide_periods(void) {
    const uint64_t duration = MATH_MS_PER_S;
    const uint64_t beyond_word = (uint64_t)UINT32_MAX + duration;
    const uint64_t wide_period = UINT64_MAX - 1;
    TEST_ASSERT_EQUAL_UINT64(mathi_repeat_u32(UINT32_MAX, MATH_MS_PER_S), mathi_repeat_u64(beyond_word, duration));
    TEST_ASSERT_EQUAL_UINT64(0, mathi_repeat_u64(wide_period, wide_period));
    TEST_ASSERT_EQUAL_UINT64(1, mathi_repeat_u64(UINT64_MAX, wide_period));
    TEST_ASSERT_EQUAL_UINT64(wide_period - 1, mathi_repeat_u64(wide_period - 1, wide_period));
    TEST_ASSERT_EQUAL_UINT64(0, mathi_repeat_u64(0, UINT64_MAX));
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX - 1, mathi_repeat_u64(UINT64_MAX - 1, UINT64_MAX));
}

static void
test_unsigned_64_clamp_covers_extremes_and_crosses_the_word_boundary(void) {
    const uint64_t boundary = (uint64_t)UINT32_MAX + 1;
    const uint64_t lo = boundary - MATH_MS_PER_S, hi = boundary + MATH_MS_PER_S;
    TEST_ASSERT_EQUAL_UINT64(lo, mathi_clamp_u64(0, lo, hi));
    TEST_ASSERT_EQUAL_UINT64(lo, mathi_clamp_u64(lo - 1, lo, hi));
    TEST_ASSERT_EQUAL_UINT64(boundary, mathi_clamp_u64(boundary, lo, hi));
    TEST_ASSERT_EQUAL_UINT64(hi, mathi_clamp_u64(hi, lo, hi));
    TEST_ASSERT_EQUAL_UINT64(hi, mathi_clamp_u64(UINT64_MAX, lo, hi));
    TEST_ASSERT_EQUAL_UINT64(0, mathi_clamp_u64(0, 0, UINT64_MAX));
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, mathi_clamp_u64(UINT64_MAX, 0, UINT64_MAX));
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX - 1, mathi_clamp_u64(UINT64_MAX, UINT64_MAX - 1, UINT64_MAX - 1));
}

/* mathi_lerp_u8() */

static void
test_lerp_hits_its_endpoints_exactly(void) {
    TEST_ASSERT_EQUAL_INT32(1000, mathi_lerp_u8(1000, 5000, 0));
    TEST_ASSERT_EQUAL_INT32(5000, mathi_lerp_u8(1000, 5000, 255));
}

static void
test_lerp_is_between_its_endpoints_throughout(void) {
    for (int u = 0; u <= 255; u++) {
        const int32_t v = mathi_lerp_u8(1000, 5000, (uint8_t)u);
        TEST_ASSERT_TRUE_MESSAGE(v >= 1000 && v <= 5000, "a lerp must never overshoot either endpoint");
    }
}

static void
test_lerp_works_the_same_shrinking_as_growing(void) {
    /* b < a: the span is negative, and the lerp should still walk from a
     * down to b rather than assuming growth. */
    TEST_ASSERT_EQUAL_INT32(5000, mathi_lerp_u8(5000, 1000, 0));
    TEST_ASSERT_EQUAL_INT32(1000, mathi_lerp_u8(5000, 1000, 255));

    int32_t last = 6000;
    for (int u = 0; u <= 255; u++) {
        const int32_t v = mathi_lerp_u8(5000, 1000, (uint8_t)u);
        TEST_ASSERT_TRUE_MESSAGE(v <= last, "lerping toward a smaller value must decrease monotonically");
        last = v;
    }
}

static void
test_lerp_handles_a_span_too_wide_for_int32_untouched(void) {
    /* (b - a) * t overflows int32 for a wide span; the int64 step keeps the result strictly between the endpoints. */
    const int32_t a = -1000000000;
    const int32_t b = 1000000000;
    TEST_ASSERT_EQUAL_INT32(a, mathi_lerp_u8(a, b, 0));
    TEST_ASSERT_EQUAL_INT32(b, mathi_lerp_u8(a, b, 255));
    const int32_t mid = mathi_lerp_u8(a, b, 128);
    TEST_ASSERT_TRUE_MESSAGE(mid > a && mid < b, "a wide span must still land strictly between its endpoints, not "
                                                 "wrap around from an overflowing intermediate");
}

static void
test_mathf_lerp(void) {
    TEST_ASSERT_EQUAL_FLOAT(0.0F, mathf_lerp(0.0F, 8.0F, 0.0F));
    TEST_ASSERT_EQUAL_FLOAT(8.0F, mathf_lerp(0.0F, 8.0F, 1.0F));
    TEST_ASSERT_EQUAL_FLOAT(6.0F, mathf_lerp(8.0F, 0.0F, 0.25F));
}

static void
test_mathi_lerp(void) {
    static const int32_t endpoints[][2] = {
        {0, INT32_MAX - 1}, {0, INT32_MAX}, {-1, INT32_MAX}, {0, INT32_MIN + 1}, {0, INT32_MIN}, {1, INT32_MIN},
    };
    const int32_t steps = 3;
    for (size_t i = 0; i < sizeof endpoints / sizeof endpoints[0]; i++) {
        const int32_t a = endpoints[i][0], b = endpoints[i][1];
        const int32_t expected = (int32_t)((int64_t)a + ((int64_t)b - a) / steps);
        TEST_ASSERT_EQUAL_INT32(expected, mathi_lerp(a, b, 1, steps));
    }
    TEST_ASSERT_EQUAL_INT32(INT32_MIN, mathi_lerp(INT32_MIN, INT32_MAX, 0, INT32_MAX));
    TEST_ASSERT_EQUAL_INT32(INT32_MAX, mathi_lerp(INT32_MIN, INT32_MAX, INT32_MAX, INT32_MAX));
    TEST_ASSERT_EQUAL_INT32(INT32_MIN, mathi_lerp(INT32_MAX, INT32_MIN, INT32_MAX, INT32_MAX));
    TEST_ASSERT_EQUAL_INT32(-1, mathi_lerp(INT32_MIN, INT32_MAX, 1, 2));
    TEST_ASSERT_EQUAL_INT32(0, mathi_lerp(INT32_MAX, INT32_MIN, 1, 2));
    TEST_ASSERT_EQUAL_INT32(1, mathi_lerp(2, -1, 1, 2));
    TEST_ASSERT_EQUAL_INT32(-1, mathi_lerp(-2, 1, 1, 2));
}

static void
test_mathi_lerp_u8(void) {
    TEST_ASSERT_EQUAL_INT32(INT32_MIN, mathi_lerp_u8(INT32_MIN, INT32_MAX, 0));
    TEST_ASSERT_EQUAL_INT32(INT32_MAX, mathi_lerp_u8(INT32_MIN, INT32_MAX, UINT8_MAX));
    TEST_ASSERT_EQUAL_INT32(1, mathi_lerp_u8(2, -1, UINT8_MAX / 2));
}

static void
test_maths_lerp(void) {
    TEST_ASSERT_EQUAL_INT32(INT16_MIN, maths_lerp(INT16_MIN, INT16_MAX, 0, INT32_MAX));
    TEST_ASSERT_EQUAL_INT32(INT16_MAX, maths_lerp(INT16_MIN, INT16_MAX, INT32_MAX, INT32_MAX));
    TEST_ASSERT_EQUAL_INT32(-1, maths_lerp(INT16_MIN, INT16_MAX, 1, 2));
    TEST_ASSERT_EQUAL_INT32(1, maths_lerp(2, -1, 1, 2));
}

static void
test_mathx_lerp(void) {
    const int32_t span = 3;
    const int32_t weight = MATHX_ONE / 4;
    TEST_ASSERT_EQUAL_INT32(1, mathx_lerp(0, span, weight));
    TEST_ASSERT_EQUAL_INT32(-1, mathx_lerp(0, -span, weight));
    TEST_ASSERT_EQUAL_INT32(-2, mathx_lerp(0, -span, MATHX_ONE - weight));
    TEST_ASSERT_EQUAL_INT32(span - 1, mathx_lerp(span, 0, weight));
    TEST_ASSERT_EQUAL_INT32(0, mathx_lerp(0, MATHX_ONE, 0));
    TEST_ASSERT_EQUAL_INT32(MATHX_ONE, mathx_lerp(0, MATHX_ONE, MATHX_ONE));
    TEST_ASSERT_EQUAL_INT32(MATHX_ONE / 2, mathx_lerp(MATHX_ONE, 0, MATHX_ONE / 2));
    TEST_ASSERT_EQUAL_INT32(INT32_MAX, mathx_lerp(INT32_MAX - 1, INT32_MAX, 2 * MATHX_ONE));
    TEST_ASSERT_EQUAL_INT32(INT32_MIN, mathx_lerp(INT32_MIN + 1, INT32_MIN, 2 * MATHX_ONE));
    TEST_ASSERT_EQUAL_INT32(-1, mathx_lerp(INT32_MIN, INT32_MAX, MATHX_ONE));
}

static void
test_mathf_repeat(void) {
    TEST_ASSERT_EQUAL_FLOAT(0.0F, mathf_repeat(8.0F, 4.0F));
    TEST_ASSERT_EQUAL_FLOAT(0.0F, mathf_repeat(-8.0F, 4.0F));
    TEST_ASSERT_EQUAL_FLOAT(3.0F, mathf_repeat(-1.0F, 4.0F));
    TEST_ASSERT_EQUAL_FLOAT(nextafterf(4.0F, 0.0F), mathf_repeat(nextafterf(4.0F, 0.0F), 4.0F));
    TEST_ASSERT_TRUE(mathf_repeat(nextafterf(4.0F, 0.0F), 4.0F) < 4.0F);
    TEST_ASSERT_EQUAL_FLOAT(0.0F, mathf_repeat(-FLT_MIN, 1.0F));
}

static float
repeat_reference(float v, float len) {
    float r = fmodf(v, len);
    if (r < 0.0F) {
        r += len;
    }
    return r < len ? r : 0.0F;
}

#define MATHF_WRAP_ULP_LIMIT 0

static void
assert_repeat_and_wrap_bits(float v, float len) {
    const float expected = repeat_reference(v, len);
    const float actual = mathf_repeat(v, len);
    if (isnan(expected)) {
        TEST_ASSERT_TRUE(isnan(actual));
    } else {
        TEST_ASSERT_UINT32_WITHIN(MATHF_WRAP_ULP_LIMIT, float_bits(expected), float_bits(actual));
    }
    const float lo = -len;
    const float hi = len;
    const float wrapped = lo + repeat_reference(v - lo, hi - lo);
    const float expected_wrap = wrapped < hi ? wrapped : lo;
    const float actual_wrap = mathf_wrap(v, lo, hi);
    if (isnan(expected_wrap)) {
        TEST_ASSERT_TRUE(isnan(actual_wrap));
    } else {
        TEST_ASSERT_UINT32_WITHIN(MATHF_WRAP_ULP_LIMIT, float_bits(expected_wrap), float_bits(actual_wrap));
    }
}

static void
test_mathf_repeat_and_wrap_reference_edges(void) {
    static const float lengths[] = {FLT_TRUE_MIN, FLT_MIN / 2.0F, FLT_MIN, 0.1F, 1.0F, 4.0F, FLT_MAX, INFINITY};
    for (size_t i = 0; i < sizeof lengths / sizeof lengths[0]; i++) {
        const float len = lengths[i];
        const float boundary = MATHF_REPEAT_FAST_QUOTIENT * len;
        const float values[] = {0.0F,
                                -0.0F,
                                len,
                                -len,
                                len * 2.0F,
                                -len * 2.0F,
                                -FLT_MIN,
                                nextafterf(len, 0.0F),
                                nextafterf(-len, 0.0F),
                                nextafterf(boundary, 0.0F),
                                boundary,
                                nextafterf(boundary, INFINITY),
                                -nextafterf(boundary, 0.0F),
                                -boundary,
                                -nextafterf(boundary, INFINITY),
                                INFINITY,
                                -INFINITY,
                                NAN};
        for (size_t j = 0; j < sizeof values / sizeof values[0]; j++) {
            assert_repeat_and_wrap_bits(values[j], len);
        }
    }
}

static void
test_mathf_repeat_and_wrap_near_quotient_guard(void) {
    static const int exponents[] = {-100, -20, 0, 20, 100};
    const int sample_count = 32;
    const float first_quotient = MATHF_REPEAT_FAST_QUOTIENT / 2.0F;
    const float last_quotient = MATHF_REPEAT_FAST_QUOTIENT - 1.0F;
    for (size_t i = 0; i < sizeof exponents / sizeof exponents[0]; i++) {
        const float len = ldexpf(1.3F, exponents[i]);
        for (int sample = 0; sample < sample_count; sample++) {
            const float k =
                first_quotient + (last_quotient - first_quotient) * (float)sample / (float)(sample_count - 1);
            const float v = k * len;
            const float values[] = {v,  nextafterf(v, -INFINITY),  nextafterf(v, INFINITY),
                                    -v, nextafterf(-v, -INFINITY), nextafterf(-v, INFINITY)};
            for (size_t j = 0; j < sizeof values / sizeof values[0]; j++) {
                assert_repeat_and_wrap_bits(values[j], len);
            }
        }
    }
}

#define REPEAT_SWEEP_SAMPLES  32768
#define REPEAT_MAGNITUDE_MASK UINT32_C(0x7fffffff)

static unsigned
assert_repeat_sample(float v, float len) {
    if (!(len > 0.0F) || !isfinite(len)) {
        return 0;
    }
    assert_repeat_and_wrap_bits(v, len);
    return 1;
}

static void
test_mathf_repeat_and_wrap_reference_sweep(void) {
    unsigned count = 0;
    static const uint32_t seeds[] = {UINT32_C(1), UINT32_C(0x12345678), UINT32_C(0xdeadbeef)};
    for (size_t seed = 0; seed < sizeof seeds / sizeof seeds[0]; seed++) {
        uint32_t state = seeds[seed];
        for (int sample = 0; sample < REPEAT_SWEEP_SAMPLES; sample++) {
            state = state * NUMBER_LCG_MULTIPLIER + NUMBER_LCG_INCREMENT;
            const float v = float_from_bits(state);
            state = state * NUMBER_LCG_MULTIPLIER + NUMBER_LCG_INCREMENT;
            const float len = float_from_bits(state & REPEAT_MAGNITUDE_MASK);
            count += assert_repeat_sample(v, len);
        }
    }
    printf("MATHERROR name=mathf_repeat type=f max=0 unit=ulp method=sweep inputs=%u\n", count);
}

static void
test_mathf_wrap(void) {
    const float lo = 1.5F, hi = 4.0F;
    TEST_ASSERT_EQUAL_FLOAT(3.5F, mathf_wrap(1.0F, lo, hi));
    const float rounded = mathf_wrap(nextafterf(1.0F, 0.0F), 1.0F, 2.0F);
    TEST_ASSERT_TRUE(rounded < 2.0F);
    TEST_ASSERT_EQUAL_FLOAT(1.0F, rounded);
    TEST_ASSERT_EQUAL_FLOAT(-4.0F, mathf_wrap(0.0F, -4.0F, -2.0F));
    TEST_ASSERT_EQUAL_FLOAT(-3.0F, mathf_wrap(-5.0F, -4.0F, -2.0F));
    TEST_ASSERT_EQUAL_FLOAT(-4.0F, mathf_wrap(-2.0F, -4.0F, -2.0F));
    TEST_ASSERT_EQUAL_FLOAT(0.0F, mathf_wrap(-FLT_MIN, 0.0F, 1.0F));
    TEST_ASSERT_EQUAL_FLOAT(nextafterf(4.0F, 0.0F), mathf_wrap(nextafterf(4.0F, 0.0F), 0.0F, 4.0F));
    TEST_ASSERT_TRUE(mathf_wrap(nextafterf(4.0F, 0.0F), 0.0F, 4.0F) < 4.0F);
}

#define DEFINE_SCALAR_BOUND_TESTS(P, A, LO, HI)                                                                        \
    static void test_##P##_min(void) {                                                                                 \
        A(LO, P##_min(LO, HI));                                                                                        \
        A(LO, P##_min(HI, LO));                                                                                        \
    }                                                                                                                  \
    static void test_##P##_max(void) {                                                                                 \
        A(HI, P##_max(LO, HI));                                                                                        \
        A(HI, P##_max(HI, LO));                                                                                        \
    }                                                                                                                  \
    static void test_##P##_clamp(void) {                                                                               \
        A(LO, P##_clamp(LO, LO, HI));                                                                                  \
        A(HI, P##_clamp(HI, LO, HI));                                                                                  \
        A(0, P##_clamp(0, LO, HI));                                                                                    \
        A(0, P##_clamp(LO, 0, HI));                                                                                    \
        A(0, P##_clamp(HI, LO, 0));                                                                                    \
    }

#define DEFINE_SCALAR_INTEGER_RANGE_TESTS(P, LO, HI)                                                                   \
    static void test_##P##_repeat(void) {                                                                              \
        TEST_ASSERT_EQUAL_INT32(0, P##_repeat(-8, 4));                                                                 \
        TEST_ASSERT_EQUAL_INT32(3, P##_repeat(-1, 4));                                                                 \
        TEST_ASSERT_EQUAL_INT32(3, P##_repeat(3, 4));                                                                  \
        TEST_ASSERT_EQUAL_INT32(0, P##_repeat(8, 4));                                                                  \
        TEST_ASSERT_EQUAL_INT32(0, P##_repeat(LO, 1));                                                                 \
    }                                                                                                                  \
    static void test_##P##_wrap(void) {                                                                                \
        TEST_ASSERT_EQUAL_INT32(-3, P##_wrap(-5, -4, -2));                                                             \
        TEST_ASSERT_EQUAL_INT32(-4, P##_wrap(-2, -4, -2));                                                             \
        TEST_ASSERT_EQUAL_INT32(LO, P##_wrap(HI, LO, HI));                                                             \
    }

DEFINE_SCALAR_BOUND_TESTS(mathf, TEST_ASSERT_EQUAL_FLOAT, -4.0F, 4.0F)
DEFINE_SCALAR_BOUND_TESTS(maths, TEST_ASSERT_EQUAL_INT32, INT16_MIN, INT16_MAX)
DEFINE_SCALAR_BOUND_TESTS(mathx, TEST_ASSERT_EQUAL_INT32, INT32_MIN, INT32_MAX)
DEFINE_SCALAR_INTEGER_RANGE_TESTS(maths, INT16_MIN, INT16_MAX)
DEFINE_SCALAR_INTEGER_RANGE_TESTS(mathx, INT32_MIN, INT32_MAX)

static void
test_mathf_repeat_preserves_decimal_remainders(void) {
    static const float values[] = {20561.0F, -20561.0F, 3.3F, -3.3F};
    const float len = 0.001F, tiny = -0.0000001F;
    TEST_ASSERT_TRUE(mathf_repeat(tiny, len) == tiny + len);
    for (size_t i = 0; i < sizeof values / sizeof values[0]; i++) {
        const float remainder = fmodf(values[i], len);
        const float expected = remainder < 0 ? remainder + len : remainder;
        const float got = mathf_repeat(values[i], len);
        TEST_ASSERT_TRUE(got >= 0 && got < len);
        TEST_ASSERT_TRUE(got == expected);
    }
}

static void
test_float_ranges_with_decimal_lengths(void) {
    static const float values[] = {3.3F, -3.3F, 6.6F, -6.6F};
    const float len = 1.1F, lo = 0, hi = len;
    for (size_t i = 0; i < sizeof values / sizeof values[0]; i++) {
        const float r = mathf_repeat(values[i], len), w = mathf_wrap(values[i], lo, hi);
        TEST_ASSERT_TRUE(r >= 0 && r < len);
        TEST_ASSERT_TRUE(w >= lo && w < hi);
    }
}

static void
test_rsqrt_tiny_inputs_preserve_exact_bits(void) {
    const float inputs[] = {
        0.0F, FLT_TRUE_MIN, FLT_MIN / 2.0F, nextafterf(FLT_MIN, 0.0F), FLT_MIN, nextafterf(FLT_MIN, 1.0F)};
    for (size_t i = 0; i < sizeof inputs / sizeof inputs[0]; i++) {
        const float x = float_from_bits(float_bits(inputs[i]));
        const uint32_t expected_bits = float_bits(1.0F / sqrtf(x));
        const uint32_t actual_bits = float_bits(mathf_rsqrt(x));
        if (x == 0.0F) {
            TEST_ASSERT_EQUAL_HEX32(float_bits(INFINITY), actual_bits);
        }
        const uint32_t scalar_error =
            actual_bits > expected_bits ? actual_bits - expected_bits : expected_bits - actual_bits;
        TEST_ASSERT_LESS_OR_EQUAL_UINT32(x < FLT_MIN ? 0 : MATHF_RSQRT_ULP_LIMIT, scalar_error);
        vec3f_t v = {sqrtf(x), 0.0F, 0.0F};
        if (x == nextafterf(FLT_MIN, 0.0F)) {
            v.x = nextafterf(sqrtf(FLT_MIN), 0.0F);
        } else if (x == nextafterf(FLT_MIN, 1.0F)) {
            v.x = sqrtf(FLT_MIN);
            v.y = sqrtf(FLT_TRUE_MIN);
        }
        const float d = vec3f_dot(v, v);
        TEST_ASSERT_EQUAL_HEX32(float_bits(x), float_bits(d));
        const float k = 1.0F / sqrtf(d);
        const vec3f_t expected = vec3f_scale(v, k);
        const vec3f_t actual = vec3f_normalize(v);
        float quotient;
        const vec3f_t divided = vec3f_normalize_divide(v, 1.0F, &quotient);
        const quatf_t q = {v.x, v.y, v.z, 0.0F};
        const quatf_t want = {q.x * k, q.y * k, q.z * k, q.w * k};
        const quatf_t got = quatf_normalize(q);
        if (d < FLT_MIN) {
            TEST_ASSERT_EQUAL_MEMORY(&expected, &actual, sizeof actual);
            TEST_ASSERT_EQUAL_MEMORY(&expected, &divided, sizeof divided);
            TEST_ASSERT_EQUAL_HEX32(float_bits(k), float_bits(quotient));
            TEST_ASSERT_EQUAL_MEMORY(&want, &got, sizeof got);
        } else {
            const float expected_components[] = {expected.x, expected.y, expected.z, expected.x, expected.y, expected.z,
                                                 k,          want.x,     want.y,     want.z,     want.w};
            const float actual_components[] = {actual.x, actual.y, actual.z, divided.x, divided.y, divided.z,
                                               quotient, got.x,    got.y,    got.z,     got.w};
            for (size_t c = 0; c < sizeof expected_components / sizeof expected_components[0]; c++) {
                const uint32_t a = float_bits(expected_components[c]);
                const uint32_t b = float_bits(actual_components[c]);
                TEST_ASSERT_LESS_OR_EQUAL_UINT32(MATHF_NORMALIZE_ULP_LIMIT, a > b ? a - b : b - a);
            }
        }
    }
}

#define RSQRT_FRACTION_BITS   23
#define RSQRT_EXPONENT_BIAS   127
#define RSQRT_SIGNIFICAND_ONE (UINT32_C(1) << RSQRT_FRACTION_BITS)
#define RSQRT_FRACTION_MASK   (RSQRT_SIGNIFICAND_ONE - 1)
#define RSQRT_WORD_BITS       32
#if !defined(ESP_PLATFORM) || (defined(__XTENSA__) && CONFIG_MATH_FPU_RECIP)
#define RSQRT_EXPONENT_MAX    254
#define RSQRT_MANTISSA_STRIDE 65537
#endif
#ifndef ESP_PLATFORM
#define RSQRT_DENSE_STRIDE      251
#define RSQRT_SEED_RADIUS       4
#define RSQRT_DOUBLE_TIE_MARGIN 8
#endif

/* The 24-by-25-by-25-bit product needs three words; no rounded float reference enters the bracket. */
static int
rsqrt_product_compare(uint32_t input, uint32_t significand, int exponent) {
    const uint32_t mantissa = (input & RSQRT_FRACTION_MASK) | RSQRT_SIGNIFICAND_ONE;
    const uint64_t square = (uint64_t)significand * significand;
    const uint64_t low = (uint64_t)(uint32_t)square * mantissa;
    const uint64_t high = (square >> RSQRT_WORD_BITS) * mantissa + (low >> RSQRT_WORD_BITS);
    const int power =
        3 * (RSQRT_EXPONENT_BIAS + RSQRT_FRACTION_BITS) - (int)(input >> RSQRT_FRACTION_BITS) - 2 * exponent;
    const uint64_t target = UINT64_C(1) << (power - RSQRT_WORD_BITS);
    return high < target ? -1 : (high > target || (uint32_t)low != 0 ? 1 : 0);
}

static int
rsqrt_candidate_compare(uint32_t input, uint32_t candidate) {
    return rsqrt_product_compare(input, (candidate & RSQRT_FRACTION_MASK) | RSQRT_SIGNIFICAND_ONE,
                                 (int)(candidate >> RSQRT_FRACTION_BITS));
}

static uint32_t
rsqrt_reference_bits(uint32_t input, uint32_t candidate) {
    while (rsqrt_candidate_compare(input, candidate) > 0) {
        candidate--;
    }
    while (rsqrt_candidate_compare(input, candidate + 1) <= 0) {
        candidate++;
    }
    const uint32_t upper = candidate + 1;
    const int exponent = (int)(candidate >> RSQRT_FRACTION_BITS);
    const uint32_t lower_mantissa = (candidate & RSQRT_FRACTION_MASK) | RSQRT_SIGNIFICAND_ONE;
    const uint32_t upper_mantissa = ((upper & RSQRT_FRACTION_MASK) | RSQRT_SIGNIFICAND_ONE)
                                    << ((upper >> RSQRT_FRACTION_BITS) - (uint32_t)exponent);
    const int midpoint = rsqrt_product_compare(input, lower_mantissa + upper_mantissa, exponent - 1);
    return midpoint < 0 || (midpoint == 0 && (candidate & 1)) ? upper : candidate;
}

#ifndef ESP_PLATFORM
static void
assert_rsqrt_integer_bracket_midpoints(void) {
    /* A float midpoint has an odd significand greater than one; its square times a binary input cannot be one. */
    for (int exponent = 1; exponent <= RSQRT_EXPONENT_MAX; exponent++) {
        const uint32_t stride = exponent == RSQRT_EXPONENT_BIAS || exponent == RSQRT_EXPONENT_BIAS + 1
                                    ? RSQRT_DENSE_STRIDE
                                    : RSQRT_MANTISSA_STRIDE;
        for (uint32_t mantissa = 0; mantissa < RSQRT_SIGNIFICAND_ONE; mantissa += stride) {
            const uint32_t input = ((uint32_t)exponent << RSQRT_FRACTION_BITS) | mantissa;
            const double reference = 1.0 / sqrt((double)float_from_bits(input));
            const float rounded = (float)reference;
            const float adjacent = nextafterf(rounded, reference < (double)rounded ? 0.0F : INFINITY);
            const double midpoint = ((double)rounded + (double)adjacent) * 0.5;
            if (fabs(reference - midpoint) <= RSQRT_DOUBLE_TIE_MARGIN * DBL_EPSILON * reference) {
                continue;
            }
            const uint32_t expected = float_bits(rounded);
            for (int offset = -RSQRT_SEED_RADIUS; offset <= RSQRT_SEED_RADIUS; offset++) {
                TEST_ASSERT_EQUAL_HEX32(expected, rsqrt_reference_bits(input, expected + offset));
            }
        }
    }
}
#endif

static void
test_rsqrt_integer_bracket_rounds_exact_roots_and_midpoints(void) {
    static const float inputs[] = {1.0F, 2.0F, 4.0F, FLT_MIN, FLT_MAX};
    for (size_t i = 0; i < sizeof inputs / sizeof inputs[0]; i++) {
        const uint32_t guess = float_bits(1.0F / sqrtf(inputs[i]));
        const uint32_t result = rsqrt_reference_bits(float_bits(inputs[i]), guess);
#ifndef ESP_PLATFORM
        const float exact = (float)(1.0 / sqrt((double)inputs[i]));
        TEST_ASSERT_EQUAL_HEX32(float_bits(exact), result);
#endif
        TEST_ASSERT_EQUAL_HEX32(result, rsqrt_reference_bits(float_bits(inputs[i]), guess - 1));
        TEST_ASSERT_EQUAL_HEX32(result, rsqrt_reference_bits(float_bits(inputs[i]), guess + 1));
    }
#ifndef ESP_PLATFORM
    assert_rsqrt_integer_bracket_midpoints();
#endif
}

#if defined(ESP_PLATFORM) && defined(__XTENSA__) && CONFIG_MATH_FPU_RECIP
#define RSQRT_DIRECTION_RADIUS 4
#define RSQRT_MAGNITUDE_MIN    -70
#define RSQRT_MAGNITUDE_MAX    60
#define RSQRT_MAGNITUDE_STEP   5
#define RSQRT_QUATERNION_STEPS 32

static uint32_t
rsqrt_component_error(float expected, float actual) {
    const uint32_t a = float_bits(expected);
    const uint32_t b = float_bits(actual);
    if (expected == actual) {
        return 0;
    }
    return a > b ? a - b : b - a;
}

static quatf_t
rsqrt_exact_quaternion(quatf_t q) {
    const float d = ((q.x * q.x + q.y * q.y) + q.z * q.z) + q.w * q.w;
    const float k = 1.0F / sqrtf(d);
    return (quatf_t){q.x * k, q.y * k, q.z * k, q.w * k};
}

static void
rsqrt_track_component(float expected, float actual, uint32_t* maximum) {
    const uint32_t error = rsqrt_component_error(expected, actual);
    if (error > *maximum) {
        *maximum = error;
    }
}

static void
rsqrt_normalize_direction(vec3f_t direction, uint32_t* maximum) {
    for (int exponent = RSQRT_MAGNITUDE_MIN; exponent <= RSQRT_MAGNITUDE_MAX; exponent += RSQRT_MAGNITUDE_STEP) {
        const vec3f_t v = vec3f_scale(direction, ldexpf(1.0F, exponent));
        const vec3f_t want = vec3f_scale(v, 1.0F / sqrtf(vec3f_dot(v, v)));
        const vec3f_t got = vec3f_normalize(v);
        rsqrt_track_component(want.x, got.x, maximum);
        rsqrt_track_component(want.y, got.y, maximum);
        rsqrt_track_component(want.z, got.z, maximum);
    }
}

static void
rsqrt_nlerp_direction(vec3f_t direction, uint32_t* maximum) {
    const quatf_t a = rsqrt_exact_quaternion((quatf_t){direction.x, direction.y, direction.z, 1.0F});
    for (int pair = 0; pair <= RSQRT_QUATERNION_STEPS; pair++) {
        const float angle = (float)pair * MATH_TAU / RSQRT_QUATERNION_STEPS;
        quatf_t b = {sinf(angle), 0.0F, 0.0F, cosf(angle)};
        const quatf_t input_b = b;
        const float dot = ((a.x * b.x + a.y * b.y) + a.z * b.z) + a.w * b.w;
        if (dot < 0.0F) {
            b = (quatf_t){-b.x, -b.y, -b.z, -b.w};
        }
        for (int step = 0; step <= RSQRT_QUATERNION_STEPS; step++) {
            const float t = (float)step / RSQRT_QUATERNION_STEPS;
            const float wa = 1.0F - t;
            const quatf_t want = rsqrt_exact_quaternion(
                (quatf_t){a.x * wa + b.x * t, a.y * wa + b.y * t, a.z * wa + b.z * t, a.w * wa + b.w * t});
            const quatf_t got = quatf_nlerp(a, input_b, t);
            rsqrt_track_component(want.x, got.x, maximum);
            rsqrt_track_component(want.y, got.y, maximum);
            rsqrt_track_component(want.z, got.z, maximum);
            rsqrt_track_component(want.w, got.w, maximum);
        }
    }
}

static void
test_rsqrt_normalize_and_nlerp_component_sweeps(void) {
    uint32_t normalize_error = 0;
    uint32_t nlerp_error = 0;
    for (int x = -RSQRT_DIRECTION_RADIUS; x <= RSQRT_DIRECTION_RADIUS; x++) {
        for (int y = -RSQRT_DIRECTION_RADIUS; y <= RSQRT_DIRECTION_RADIUS; y++) {
            for (int z = -RSQRT_DIRECTION_RADIUS; z <= RSQRT_DIRECTION_RADIUS; z++) {
                if (x == 0 && y == 0 && z == 0) {
                    continue;
                }
                const vec3f_t direction = {(float)x, (float)y, (float)z};
                rsqrt_normalize_direction(direction, &normalize_error);
                rsqrt_nlerp_direction(direction, &nlerp_error);
            }
        }
    }
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(MATHF_NORMALIZE_ULP_LIMIT, normalize_error);
    printf("MATHERROR name=normalize (vec3) type=f max=%lu unit=ulp method=sweep\n", (unsigned long)normalize_error);
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(MATHF_NLERP_ULP_LIMIT, nlerp_error);
    printf("MATHERROR name=nlerp type=f max=%lu unit=ulp method=sweep\n", (unsigned long)nlerp_error);
}

static inline __attribute__((always_inline)) void
rsqrt_measure(uint32_t input, uint32_t* max_ulp, float* max_relative, double* bins) {
    const float value = float_from_bits(input);
    const uint32_t got = float_bits(mathf_rsqrt(value));
    const uint32_t reference = rsqrt_reference_bits(input, got);
    const uint32_t error = got > reference ? got - reference : reference - got;
    if (error > *max_ulp) {
        *max_ulp = error;
    }
    const float approximate = mathf_rsqrt_approx(value);
    const float lower = float_from_bits(reference - 1);
    const float upper = float_from_bits(reference + 1);
    const float relative =
        nextafterf(fmaxf(fabsf(approximate / lower - 1.0F), fabsf(approximate / upper - 1.0F)) + FLT_EPSILON, INFINITY);
    if (relative > *max_relative) {
        *max_relative = relative;
    }
    const unsigned bin = fast_path_period_bin(value, FAST_PATH_RSQRT_OCTAVES);
    const bool refine_ulp = error != 0 || bins[bin] < FAST_PATH_ROUNDING_ULP;
    const bool refine_relative = (double)relative > bins[FAST_PATH_RSQRT_BINS + bin];
    if (refine_ulp || refine_relative) {
        const double exact = 1.0 / sqrt((double)value);
        if (refine_ulp) {
            bins[bin] = fmax(bins[bin], fast_path_fractional_ulp(float_from_bits(got), exact));
        }
        if (refine_relative) {
            bins[FAST_PATH_RSQRT_BINS + bin] =
                fmax(bins[FAST_PATH_RSQRT_BINS + bin], fabs((double)approximate - exact) / exact);
        }
    }
}

static void
test_rsqrt_device_mantissas_and_normal_exponent_edges(void) {
    uint32_t max_ulp = 0;
    float max_relative = 0.0F;
    double* bins = calloc(2 * FAST_PATH_RSQRT_BINS, sizeof(*bins));
    TEST_ASSERT_NOT_NULL(bins);
    const uint32_t first = RSQRT_EXPONENT_BIAS << RSQRT_FRACTION_BITS;
    const uint32_t end = (RSQRT_EXPONENT_BIAS + 2) << RSQRT_FRACTION_BITS;
    for (int exponent = 1; exponent <= RSQRT_EXPONENT_MAX; exponent++) {
        const uint32_t power = (uint32_t)exponent << RSQRT_FRACTION_BITS;
        const uint32_t stride = power >= first && power < end ? 1 : RSQRT_MANTISSA_STRIDE;
        for (uint32_t mantissa = 0; mantissa < RSQRT_SIGNIFICAND_ONE; mantissa += stride) {
            rsqrt_measure(power | mantissa, &max_ulp, &max_relative, bins);
        }
        rsqrt_measure(power | RSQRT_FRACTION_MASK, &max_ulp, &max_relative, bins);
        for (int offset = -1; offset <= 1; offset++) {
            const uint32_t input = power + offset;
            if (input >= RSQRT_SIGNIFICAND_ONE) {
                rsqrt_measure(input, &max_ulp, &max_relative, bins);
            }
        }
    }
    for (unsigned bin = 0; bin < FAST_PATH_RSQRT_BINS; bin++) {
        const double centre = 1.0 + (bin + 0.5) / FAST_PATH_BINS_PER_UNIT;
        printf("MATHERRORBIN name=mathf_rsqrt type=f x=%g max=%.12g unit=ulp axis=period\n", centre, bins[bin]);
        printf("MATHERRORBIN name=mathf_rsqrt_approx type=f x=%g max=%.12g unit=relative axis=period\n", centre,
               bins[FAST_PATH_RSQRT_BINS + bin]);
    }
    free(bins);
    printf("MATHERROR name=mathf_rsqrt type=f max=%lu unit=ulp method=proof\n", (unsigned long)max_ulp);
    printf("MATHERROR name=mathf_rsqrt_approx type=f max=%g unit=relative method=proof\n", (double)max_relative);
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(MATHF_RSQRT_ULP_LIMIT, max_ulp);
    TEST_ASSERT_TRUE(max_relative <= MATHF_RSQRT_APPROX_RELATIVE_LIMIT);
}
#endif

static void
test_scalar_half_and_absolute_value(void) {
    static const float half = 0.5F;
    TEST_ASSERT_EQUAL_FLOAT(half, mathf_half());
    TEST_ASSERT_EQUAL_INT32(MATHX_ONE / 2, mathx_half());
    TEST_ASSERT_EQUAL_FLOAT(mathf_one(), mathf_abs(-mathf_one()));
    TEST_ASSERT_EQUAL_FLOAT(mathf_one(), mathf_abs(mathf_one()));
    TEST_ASSERT_EQUAL_FLOAT(mathf_zero(), mathf_abs(mathf_zero()));
    TEST_ASSERT_FALSE(signbit(mathf_abs(-mathf_zero())));
    TEST_ASSERT_EQUAL_INT32(mathx_one(), mathx_abs(-mathx_one()));
    TEST_ASSERT_EQUAL_INT32(mathx_one(), mathx_abs(mathx_one()));
    TEST_ASSERT_EQUAL_INT32(mathx_zero(), mathx_abs(mathx_zero()));
    TEST_ASSERT_EQUAL_INT32(INT32_MAX, mathx_abs(INT32_MIN));
}

static void
test_integer_square_root_edges(void) {
    enum { ROOT_LIMIT = 65535 };

    TEST_ASSERT_EQUAL_UINT32(0, mathi_isqrt64(0));
    TEST_ASSERT_EQUAL_UINT32(ROOT_LIMIT, mathi_isqrt64(UINT32_MAX));
    TEST_ASSERT_EQUAL_UINT32(ROOT_LIMIT + 1, mathi_isqrt64((uint64_t)UINT32_MAX + 1));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, mathi_isqrt64(UINT64_MAX));
    for (uint32_t n = 1; n <= ROOT_LIMIT; n++) {
        const uint32_t square = n * n;
        TEST_ASSERT_EQUAL_UINT32(n, mathi_isqrt64(square));
        TEST_ASSERT_EQUAL_UINT32(n - 1, mathi_isqrt64(square - 1));
        TEST_ASSERT_EQUAL_UINT32(n, mathi_isqrt64(square + n));
    }
}

static void
test_integer_half_chords_floor(void) {
    enum { FLOOR_LIMIT = 64 };

    for (int r = 0; r <= FLOOR_LIMIT; r++) {
        for (int d = -r - 1; d <= r + 1; d++) {
            const int h = r * r - d * d;
            const int x = mathi_half_chord(r, d);
            TEST_ASSERT_EQUAL_INT(x, mathi_half_chord(r, -d));
            if (h <= 0) {
                TEST_ASSERT_EQUAL_INT(0, x);
            } else {
                TEST_ASSERT_TRUE(x * x <= h);
                TEST_ASSERT_TRUE((x + 1) * (x + 1) > h);
            }
        }
    }
}

static void
test_integer_half_chords_round(void) {
    enum { ROUND_LIMIT = 512 };

    for (int r = 0; r <= ROUND_LIMIT; r++) {
        for (int d = -r - 1; d <= r + 1; d++) {
            const int h = r * r - d * d;
            const int want = h > 0 ? (int)lroundf(sqrtf((float)h)) : 0;
            TEST_ASSERT_EQUAL_INT(want, mathi_half_chord_round(r, d));
        }
    }
}

static void
test_integer_half_chords_int16_radius_edges(void) {
    enum { FLOOR_LIMIT = 64, RADIUS = INT16_MAX };

    TEST_ASSERT_EQUAL_INT(RADIUS, mathi_half_chord(RADIUS, 0));
    TEST_ASSERT_EQUAL_INT(RADIUS, mathi_half_chord_round(RADIUS, 0));
    TEST_ASSERT_EQUAL_INT(0, mathi_half_chord(-RADIUS, 0));
    for (int d = RADIUS - FLOOR_LIMIT; d <= RADIUS + 1; d++) {
        const int h = RADIUS * RADIUS - d * d;
        int floor = 0;
        while ((floor + 1) * (floor + 1) <= h) {
            floor++;
        }
        const int rounded = floor + (h - floor * floor > floor);
        TEST_ASSERT_EQUAL_INT(floor, mathi_half_chord(RADIUS, d));
        TEST_ASSERT_EQUAL_INT(floor, mathi_half_chord(RADIUS, -d));
        TEST_ASSERT_EQUAL_INT(rounded, mathi_half_chord_round(RADIUS, d));
        TEST_ASSERT_EQUAL_INT(rounded, mathi_half_chord_round(RADIUS, -d));
    }
    TEST_ASSERT_EQUAL_INT(0, mathi_half_chord(-RADIUS, FLOOR_LIMIT));
    TEST_ASSERT_EQUAL_INT(0, mathi_half_chord_round(-RADIUS, FLOOR_LIMIT));
}

static void
test_integer_half_chords_int32(void) {
    enum { LARGE_RADIUS = 80000, POWER_RADIUS = 1 << 30 };

    const int radii[] = {LARGE_RADIUS, POWER_RADIUS, INT32_MAX};
    for (size_t i = 0; i < sizeof radii / sizeof radii[0]; i++) {
        const int r = radii[i];
        const int offsets[] = {INT32_MIN, -r, -r + 1, -r / 2, 0, r / 2, r - 1, r, INT32_MAX};
        for (size_t j = 0; j < sizeof offsets / sizeof offsets[0]; j++) {
            const int d = offsets[j];
            const int64_t h = (int64_t)r * r - (int64_t)d * d;
            int64_t lo = 0, hi = (int64_t)r + 1;
            while (hi - lo > 1) {
                const int64_t mid = lo + (hi - lo) / 2;
                if (mid * mid <= h) {
                    lo = mid;
                } else {
                    hi = mid;
                }
            }
            const int rounded = (int)(lo + (h - lo * lo > lo));
            TEST_ASSERT_EQUAL_INT((int)lo, mathi_half_chord(r, d));
            TEST_ASSERT_EQUAL_INT(rounded, mathi_half_chord_round(r, d));
        }
    }
}

/* The double-precision reference runs on the host only: the device's FPU is single precision. */
#ifndef ESP_PLATFORM
#define FIXED_RSQRT_SWEEP_STRIDE    (UINT32_C(1) << 20)
#define FIXED_NLERP_COMPONENT_ERROR 8.0
#define FIXED_NLERP_STEPS           32

static void
test_fixed_rsqrt_seed_provenance(void) {
    const double segments = 1 << MATHX_RSQRT_SEED_BITS;
    const double first = MATHX_RSQRT_MIN / ldexp(1.0, MATHX_RSQRT_WORD_BITS - MATHX_RSQRT_SEED_BITS);
    for (size_t i = 0; i < sizeof mathx_rsqrt_seeds / sizeof mathx_rsqrt_seeds[0]; i++) {
        const double midpoint = (first + i + 0.5) / segments;
        const uint32_t seed = (uint32_t)llround(ldexp(1.0, MATHX_RSQRT_SHIFT) / sqrt(midpoint));
        TEST_ASSERT_EQUAL_UINT32(seed, mathx_rsqrt_seeds[i]);
    }
}

static double
assert_fixed_rsqrt_reference(uint32_t m) {
    const double exact = ldexp(1.0, MATHX_RSQRT_SHIFT + MATHX_RSQRT_WORD_BITS / 2) / sqrt((double)m);
    const double relative = fabs(mathx_rsqrt_q30(m) / exact - 1.0);
    TEST_ASSERT_TRUE(relative <= MATHX_RSQRT_Q30_RELATIVE_LIMIT);
    const uint32_t expected = mathx_rsqrt_q30_table(m);
    TEST_ASSERT_EQUAL_UINT32(expected, mathx_rsqrt_q30(m));
    const uint32_t seeds[] = {
        (uint32_t)floor(exact * (1.0 - MATHX_RSQRT_FPU_SEED_RELATIVE_LIMIT)),
        (uint32_t)ceil(exact * (1.0 + MATHX_RSQRT_FPU_SEED_RELATIVE_LIMIT)),
    };
    for (size_t i = 0; i < sizeof seeds / sizeof seeds[0]; i++) {
        TEST_ASSERT_EQUAL_UINT32(expected, mathx_rsqrt_bracket(m, mathx_rsqrt_newton(m, seeds[i])));
    }
    return relative;
}

static void
test_fixed_rsqrt_seed_boundaries_and_stride(void) {
    const uint64_t end = UINT64_C(1) << MATHX_RSQRT_WORD_BITS;
    const uint32_t segment = UINT32_C(1) << (MATHX_RSQRT_WORD_BITS - MATHX_RSQRT_SEED_BITS);
    for (uint64_t m = MATHX_RSQRT_MIN; m <= end; m += segment) {
        for (int delta = -2; delta <= 2; delta++) {
            const int64_t value = (int64_t)m + delta;
            if (value >= MATHX_RSQRT_MIN && value < (int64_t)end) {
                assert_fixed_rsqrt_reference((uint32_t)value);
            }
        }
    }
    double maximum = 0.0;
    for (uint64_t start = MATHX_RSQRT_MIN; start < end; start += segment) {
        double bin_maximum = 0.0;
        for (uint64_t m = start; m < start + segment; m += FIXED_RSQRT_SWEEP_STRIDE) {
            const double relative = assert_fixed_rsqrt_reference((uint32_t)m);
            bin_maximum = fmax(bin_maximum, relative);
        }
        maximum = fmax(maximum, bin_maximum);
        printf("MATHERRORBIN name=mathx_rsqrt_q30 type=x x=%g max=%.12g unit=relative axis=period\n",
               (double)(start + segment / 2) / MATHX_RSQRT_MIN, bin_maximum);
    }
    printf("MATHERROR name=mathx_rsqrt_q30 type=x max=%.12g unit=relative method=proof\n", maximum);
}

static void
test_fixed_scalar_rsqrt_bound(void) {
    for (int bit = 0; bit < MATHX_RSQRT_WORD_BITS - 1; bit++) {
        const int32_t base = (int32_t)(UINT32_C(1) << bit);
        for (int delta = -2; delta <= 2; delta++) {
            const int64_t v = (int64_t)base + delta;
            if (v <= 0 || v > INT32_MAX) {
                continue;
            }
            const double exact = MATHX_ONE / sqrt((double)v / MATHX_ONE);
            TEST_ASSERT_TRUE(fabs(mathx_rsqrt((int32_t)v) - exact)
                             <= NORMALIZE_ROUNDING_ERROR + exact * MATHX_RSQRT_RELATIVE_LIMIT);
        }
    }
    for (int64_t v = 1; v <= INT32_MAX; v += FIXED_RSQRT_SWEEP_STRIDE) {
        const double exact = MATHX_ONE / sqrt((double)v / MATHX_ONE);
        TEST_ASSERT_TRUE(fabs(mathx_rsqrt((int32_t)v) - exact)
                         <= NORMALIZE_ROUNDING_ERROR + exact * MATHX_RSQRT_RELATIVE_LIMIT);
    }
    const double exact = MATHX_ONE / sqrt((double)INT32_MAX / MATHX_ONE);
    TEST_ASSERT_TRUE(fabs(mathx_rsqrt(INT32_MAX) - exact)
                     <= NORMALIZE_ROUNDING_ERROR + exact * MATHX_RSQRT_RELATIVE_LIMIT);
}

static void
assert_unit_components(const double* reference, const int32_t* got, size_t n, double bound) {
    double sum = 0;
    for (size_t c = 0; c < n; c++) {
        sum += reference[c] * reference[c];
    }
    const double length = sqrt(sum);
    for (size_t c = 0; c < n; c++) {
        TEST_ASSERT_TRUE(fabs(got[c] - reference[c] * MATHX_ONE / length) <= bound);
    }
}

static void
test_fixed_quaternion_normalize_extremes(void) {
    const quatx_t cases[] = {{200 * MATHX_ONE, 0, 0, 0},
                             {1, 0, 0, 0},
                             {INT32_MIN, INT32_MIN, INT32_MIN, INT32_MIN},
                             {INT32_MAX, 1, -1, 2},
                             {MATHX_ONE / 2, MATHX_ONE / 2, 0, 0}};
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        const quatx_t q = cases[i];
        const quatx_t got = quatx_normalize(q);
        const double input[] = {q.x, q.y, q.z, q.w};
        const int32_t output[] = {got.x, got.y, got.z, got.w};
        assert_unit_components(input, output, sizeof input / sizeof input[0], NORMALIZE_COMPONENT_ERROR);
    }
}

static void
assert_fixed_nlerp_sample(quatx_t a, quatx_t b, int32_t t) {
    const int32_t av[] = {a.x, a.y, a.z, a.w};
    const int32_t bv[] = {b.x, b.y, b.z, b.w};
    double dot = 0;
    for (size_t c = 0; c < sizeof av / sizeof av[0]; c++) {
        dot += (double)av[c] * bv[c];
    }
    double blend[sizeof av / sizeof av[0]];
    for (size_t c = 0; c < sizeof av / sizeof av[0]; c++) {
        blend[c] = (av[c] * (double)(MATHX_ONE - t) + (dot < 0 ? -1 : 1) * bv[c] * (double)t) / MATHX_ONE;
    }
    const quatx_t got = quatx_nlerp(a, b, t);
    const int32_t output[] = {got.x, got.y, got.z, got.w};
    assert_unit_components(blend, output, sizeof blend / sizeof blend[0], FIXED_NLERP_COMPONENT_ERROR);
}

static void
test_fixed_nlerp_unit_sweep(void) {
    const quatx_t inputs[] = {{MATHX_ONE, 0, 0, 0},
                              {0, MATHX_ONE, 0, 0},
                              {0, 0, MATHX_ONE, 0},
                              {0, 0, 0, MATHX_ONE},
                              {MATHX_ONE / 2, -MATHX_ONE / 2, MATHX_ONE / 2, -MATHX_ONE / 2},
                              {-MATHX_ONE / 2, MATHX_ONE / 2, MATHX_ONE / 2, MATHX_ONE / 2},
                              {3 * MATHX_ONE / 5, 4 * MATHX_ONE / 5, 0, 0},
                              {-4 * MATHX_ONE / 5, 0, 0, 3 * MATHX_ONE / 5}};
    for (size_t a = 0; a < sizeof inputs / sizeof inputs[0]; a++) {
        for (size_t b = 0; b < sizeof inputs / sizeof inputs[0]; b++) {
            for (int step = 0; step <= FIXED_NLERP_STEPS; step++) {
                const int32_t t =
                    step * (MATHX_ONE / FIXED_NLERP_STEPS) + (step > 0 && step < FIXED_NLERP_STEPS ? 1 : 0);
                assert_fixed_nlerp_sample(inputs[a], inputs[b], t);
            }
        }
    }
}

#endif

static void
test_fixed_scalar_rsqrt_nonpositive(void) {
    TEST_ASSERT_EQUAL_INT32(INT32_MAX, mathx_rsqrt(0));
    TEST_ASSERT_EQUAL_INT32(INT32_MAX, mathx_rsqrt(-1));
    TEST_ASSERT_EQUAL_INT32(INT32_MAX, mathx_rsqrt(INT32_MIN));
}

static void
test_fixed_quaternion_normalize_zero_and_unit_path(void) {
    const quatx_t zero = quatx_normalize((quatx_t){0, 0, 0, 0});
    TEST_ASSERT_TRUE(zero.x == 0 && zero.y == 0 && zero.z == 0 && zero.w == 0);
    const quatx_t near_unit = {MATHX_ONE / 2, MATHX_ONE / 2, 0, 0};
    const quatx_t unit = quatx_unit_normalize(near_unit);
    const quatx_t safe = quatx_normalize(near_unit);
    TEST_ASSERT_EQUAL_INT32(safe.x, unit.x);
    TEST_ASSERT_EQUAL_INT32(safe.y, unit.y);
    TEST_ASSERT_EQUAL_INT32(safe.z, unit.z);
    TEST_ASSERT_EQUAL_INT32(safe.w, unit.w);
}

#define TRIG_SIN_LSB_LIMIT  5

#define FAST_PATH_PHASE_BIN (TRIG_TURN / (sizeof trig_sin_quarter / sizeof trig_sin_quarter[0] - 1))

static void
test_trig_sin_error_bins(void) {
    float maximum = 0.0F;
    for (uint32_t start = 0; start < TRIG_TURN; start += FAST_PATH_PHASE_BIN) {
        float bin_maximum = 0.0F;
        for (uint32_t phase = start; phase < start + FAST_PATH_PHASE_BIN; phase++) {
            const float angle = (float)phase * (MATH_TAU / TRIG_TURN);
            const float expected = sinf(angle) * (1 << TRIG_SIN_SHIFT);
            const float error = fabsf((float)trig_sin((uint16_t)phase) - expected);
            if (error > bin_maximum) {
                bin_maximum = error;
            }
        }
        if (bin_maximum > maximum) {
            maximum = bin_maximum;
        }
        printf("MATHERRORBIN name=trig_sin type=i x=%g max=%g unit=lsb axis=phase\n",
               (double)(start + FAST_PATH_PHASE_BIN / 2) / TRIG_TURN, (double)bin_maximum);
    }
    printf("MATHERROR name=trig_sin type=i max=%g unit=lsb method=sweep\n", (double)maximum);
    TEST_ASSERT_TRUE(maximum <= TRIG_SIN_LSB_LIMIT);
}

void
suite_math_numbers(void) {
#ifndef ESP_PLATFORM
    RUN_TEST(test_fast_path_period_bins_and_fractional_ulp);
#endif
    RUN_TEST(test_trig_sin_error_bins);
    RUN_TEST(test_fixed_scalar_rsqrt_nonpositive);
    RUN_TEST(test_fixed_quaternion_normalize_zero_and_unit_path);
#ifndef ESP_PLATFORM
    RUN_TEST(test_fixed_rsqrt_seed_provenance);
    RUN_TEST(test_fixed_rsqrt_seed_boundaries_and_stride);
    RUN_TEST(test_fixed_scalar_rsqrt_bound);
    RUN_TEST(test_fixed_quaternion_normalize_extremes);
    RUN_TEST(test_fixed_nlerp_unit_sweep);
#endif
    RUN_TEST(test_integer_half_chords_int32);
    RUN_TEST(test_integer_square_root_edges);
    RUN_TEST(test_integer_half_chords_floor);
    RUN_TEST(test_integer_half_chords_round);
    RUN_TEST(test_integer_half_chords_int16_radius_edges);
    RUN_TEST(test_scalar_half_and_absolute_value);
    RUN_TEST(test_rsqrt_tiny_inputs_preserve_exact_bits);
    RUN_TEST(test_rsqrt_integer_bracket_rounds_exact_roots_and_midpoints);
#if defined(ESP_PLATFORM) && defined(__XTENSA__) && CONFIG_MATH_FPU_RECIP
    RUN_TEST(test_rsqrt_device_mantissas_and_normal_exponent_edges);
    RUN_TEST(test_rsqrt_normalize_and_nlerp_component_sweeps);
#endif
    RUN_TEST(test_mathf_lerp);
    RUN_TEST(test_mathi_lerp);
    RUN_TEST(test_mathi_lerp_u8);
    RUN_TEST(test_maths_lerp);
    RUN_TEST(test_mathx_lerp);
    RUN_TEST(test_float_ranges_with_decimal_lengths);
    RUN_TEST(test_mathf_repeat);
    RUN_TEST(test_mathf_repeat_and_wrap_reference_edges);
    RUN_TEST(test_mathf_repeat_and_wrap_reference_sweep);
    RUN_TEST(test_mathf_repeat_and_wrap_near_quotient_guard);
    RUN_TEST(test_mathf_repeat_preserves_decimal_remainders);
    RUN_TEST(test_mathf_wrap);
    RUN_TEST(test_mathf_min);
    RUN_TEST(test_mathf_max);
    RUN_TEST(test_mathf_clamp);
    RUN_TEST(test_maths_min);
    RUN_TEST(test_maths_max);
    RUN_TEST(test_maths_clamp);
    RUN_TEST(test_mathx_min);
    RUN_TEST(test_mathx_max);
    RUN_TEST(test_mathx_clamp);
    RUN_TEST(test_maths_repeat);
    RUN_TEST(test_maths_wrap);
    RUN_TEST(test_mathx_repeat);
    RUN_TEST(test_mathx_wrap);

    RUN_TEST(test_lerp_hits_its_endpoints_exactly);
    RUN_TEST(test_lerp_is_between_its_endpoints_throughout);
    RUN_TEST(test_lerp_works_the_same_shrinking_as_growing);
    RUN_TEST(test_lerp_handles_a_span_too_wide_for_int32_untouched);

    RUN_TEST(test_unsigned_64_repeat_preserves_high_clock_bits_and_wide_periods);
    RUN_TEST(test_unsigned_64_clamp_covers_extremes_and_crosses_the_word_boundary);
    RUN_TEST(test_scalar_clamp_maps_nonfinite_values_to_bounds);
    RUN_TEST(test_scalar_rounding_preserves_large_integers_and_half_away_ties);
    RUN_TEST(test_scalar_int16_rounding_saturates_and_preserves_half_neighbors);
    RUN_TEST(test_transform_matrix_refusals_and_shear_tolerance_boundary);
    RUN_TEST(test_integer_repeat_and_wrap_cover_negative_values_and_int_limits);
    RUN_TEST(test_unsigned_repeat_keeps_clock_precision_and_clamp_covers_the_range);
    RUN_TEST(test_looking_keeps_forward_at_large_positions);
    RUN_TEST(test_vec3_add_sub_scale_dot_cross_agree_across_every_number_type);
    RUN_TEST(test_vec2_ops_agree_across_every_number_type);
    RUN_TEST(test_normalize_divide_zero_and_float);
#ifndef ESP_PLATFORM
    RUN_TEST(test_fixed_normalize_saturation_edges);
#endif
    RUN_TEST(test_fixed_normalize_extreme_magnitudes);
#ifndef ESP_PLATFORM
    RUN_TEST(test_fixed_normalize_every_magnitude);
#endif
    RUN_TEST(test_fixed_normalize_matches_float_within_its_precision);
    RUN_TEST(test_fixed_quaternion_rotation_matches_float);
    RUN_TEST(test_fixed_view_from_axes_maps_the_eye_and_frame_axes);
    RUN_TEST(test_fixed_scale_rows_preserves_the_bottom_row);
    RUN_TEST(test_fixed_scale_columns_keeps_the_translation);
    RUN_TEST(test_fixed_transform_matrix_and_view_match_float);
    RUN_TEST(test_fixed_transform_look_at_matches_float);
    RUN_TEST(test_fixed_transform_builds_once_and_rebuilds_after_each_setter);
    RUN_TEST(test_round_trip_conversions_return_the_original_where_representable);
    RUN_TEST(test_fixed_point_overflow_saturates_and_divide_by_zero_follows_the_sign);
    RUN_TEST(test_maths_saturate_exhaustive);
    RUN_TEST(test_int16_arithmetic_and_conversion_saturate_at_the_int16_range);
    RUN_TEST(test_int32_dot_widens_to_int64);
    RUN_TEST(test_fixed_dot_fast_paths_floor_each_term_and_add_the_constant);
    RUN_TEST(test_fixed_matrix_apply_matches_float);
    RUN_TEST(test_fixed_affine_product_applies_the_right_matrix_first);
    RUN_TEST(test_the_narrow_dot_is_a_plain_32_bit_sum_then_a_floor_shift);
    RUN_TEST(test_fixed_multiply_and_divide_round_ties_away_from_zero_in_both_signs);
    RUN_TEST(test_fixed_divide_handles_signs_saturation_and_the_most_negative_divisor);
    RUN_TEST(test_fixed_square_root_floors_and_survives_the_largest_input);
    RUN_TEST(test_int16_negative_edges_and_float_rounding_boundaries);
    RUN_TEST(test_normalize_by_value_and_repeated_rotation_stay_unit_length);
    RUN_TEST(test_slerp_of_equal_and_opposite_quaternions_and_across_its_threshold);
    RUN_TEST(test_from_basis_picks_the_largest_diagonal_for_off_axis_half_turns);
    RUN_TEST(test_fixed_angles_negative_and_beyond_a_turn);
    RUN_TEST(test_every_swizzle_family_moves_the_named_components_for_every_number_type);
    RUN_TEST(test_a_cross_dimension_swizzle_returns_the_other_vector_type);
    RUN_TEST(test_a_vec3_from_a_vec2_and_z_keeps_every_component_for_every_number_type);
    RUN_TEST(test_the_reciprocal_is_within_an_ulp_of_the_quotient_and_usually_on_it);
}

SUITE_REGISTER(suite_math_numbers);
