/*
 * Portable suite: util/math/'s number types. The same operations run on
 * float, int32, int16 and Q16.16 vectors, and the fixed-point rotation maths
 * on Q16.16, and each is judged against the float result within its own
 * precision. Conversions round-trip, and overflow saturates where the
 * headers say it does.
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#include "suites.h"
#include "unity.h"

#include "util/math/transformf.h"
#include "util/math/transformx.h"
#include "util/math/vec_convert.h"

#define Q           65536.0F
/* Q16.16's table-driven sine is good to about 1e-3; sums of products to far
 * less, so rotation results are compared at this. */
#define FIXED_SLACK 2e-3F

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
test_fixed_transform_matrix_view_and_look_at_match_float(void) {
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
    TEST_ASSERT_FALSE(rebuilds_x(&t));
    transformx_set_position(&t, to_x(1.0F, 0.0F, 0.0F));
    TEST_ASSERT_TRUE(rebuilds_x(&t));
    transformx_set_rotation(&t, quatx_identity());
    TEST_ASSERT_TRUE(rebuilds_x(&t));
    transformx_set_scale(&t, to_x(2.0F, 2.0F, 2.0F));
    TEST_ASSERT_TRUE(rebuilds_x(&t));
    transformx_translate(&t, to_x(1.0F, 0.0F, 0.0F));
    TEST_ASSERT_TRUE(rebuilds_x(&t));
    transformx_rotate(&t, quatx_from_axis_angle(to_x(0.0F, 1.0F, 0.0F), MATHX_ONE / 4));
    TEST_ASSERT_TRUE(rebuilds_x(&t));

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

void
suite_math_numbers(void) {
    RUN_TEST(test_vec3_add_sub_scale_dot_cross_agree_across_every_number_type);
    RUN_TEST(test_vec2_ops_agree_across_every_number_type);
    RUN_TEST(test_fixed_normalize_matches_float_within_its_precision);
    RUN_TEST(test_fixed_quaternion_rotation_matches_float);
    RUN_TEST(test_fixed_transform_matrix_view_and_look_at_match_float);
    RUN_TEST(test_fixed_transform_builds_once_and_rebuilds_after_each_setter);
    RUN_TEST(test_round_trip_conversions_return_the_original_where_representable);
    RUN_TEST(test_fixed_point_overflow_saturates_and_divide_by_zero_follows_the_sign);
    RUN_TEST(test_int16_arithmetic_and_conversion_saturate_at_the_int16_range);
    RUN_TEST(test_int32_dot_widens_to_int64);
}

SUITE_REGISTER(suite_math_numbers);
