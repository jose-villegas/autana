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
#include <string.h>

#include "suites.h"
#include "transform_cache.h"
#include "unity.h"

#include "util/math/transformf.h"
#include "util/math/transformx.h"
#include "util/math/vec_convert.h"

#define Q           ((float)MATHX_ONE)
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
}

SUITE_REGISTER(suite_math_numbers);
