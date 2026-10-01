/*
 * Portable suite: util/math/matrix4i.h. The sine and the rounding of its products,
 * judged against the exact value rather than against a pinned render.
 */

#include "suites.h"
#include "unity.h"

#include <math.h>

#include "util/math/matrix4i.h"

static void
test_the_sine_is_exact_at_the_quarter_points(void) {
    TEST_ASSERT_EQUAL_INT(0, matrix4i_sin(0));
    TEST_ASSERT_EQUAL_INT(VEC4I_ONE, matrix4i_sin(VEC4I_ONE / 4));
    TEST_ASSERT_EQUAL_INT(0, matrix4i_sin(VEC4I_ONE / 2));
    TEST_ASSERT_EQUAL_INT(-VEC4I_ONE, matrix4i_sin(3 * VEC4I_ONE / 4));
    TEST_ASSERT_EQUAL_INT(VEC4I_ONE, matrix4i_cos(0));
}

static void
test_the_sine_is_within_one_unit_of_the_true_sine_over_two_turns(void) {
    for (int angle = -VEC4I_ONE; angle < VEC4I_ONE; angle++) {
        const double exact = sin(2.0 * 3.14159265358979323846 * angle / VEC4I_ONE) * VEC4I_ONE;
        TEST_ASSERT_TRUE_MESSAGE(fabs(matrix4i_sin(angle) - exact) <= 1.0, "sine off by more than one unit");
    }
}

static void
test_the_sine_is_odd_and_periodic_for_negative_angles(void) {
    for (int angle = 1; angle < VEC4I_ONE; angle++) {
        TEST_ASSERT_EQUAL_INT(-matrix4i_sin(angle), matrix4i_sin(-angle));
        TEST_ASSERT_EQUAL_INT(matrix4i_sin(angle), matrix4i_sin(angle - VEC4I_ONE));
        TEST_ASSERT_EQUAL_INT(matrix4i_sin(VEC4I_ONE / 2 - angle), matrix4i_sin(angle));
    }
}

static void
test_a_quarter_turn_about_y_is_an_exact_rotation(void) {
    matrix4i_t m;
    matrix4i_rotation(0, VEC4I_ONE / 4, 0, m);
    TEST_ASSERT_EQUAL_INT(0, m[0][0]);
    TEST_ASSERT_EQUAL_INT(0, m[2][2]);
    TEST_ASSERT_EQUAL_INT(VEC4I_ONE, m[2][0]);
    TEST_ASSERT_EQUAL_INT(-VEC4I_ONE, m[0][2]);
    TEST_ASSERT_EQUAL_INT(VEC4I_ONE, m[1][1]);
}

static void
test_a_point_transform_rounds_ties_away_from_zero(void) {
    matrix4i_t half;
    matrix4i_scale(VEC4I_ONE / 2, VEC4I_ONE / 2, VEC4I_ONE / 2, half);
    vec4i_t point = {3, -3, 5, VEC4I_ONE};
    matrix4i_transform_point(&point, half);
    TEST_ASSERT_EQUAL_INT(2, point.x);
    TEST_ASSERT_EQUAL_INT(-2, point.y);
    TEST_ASSERT_EQUAL_INT(3, point.z);
}

static void
test_a_matrix_product_rounds_to_nearest(void) {
    matrix4i_t half, other;
    matrix4i_scale(VEC4I_ONE / 2, VEC4I_ONE / 2, VEC4I_ONE / 2, half);
    matrix4i_scale(3, 3, 3, other);
    matrix4i_mul(half, other);
    TEST_ASSERT_EQUAL_INT(2, half[0][0]);
}

void
run_matrix4i_suite(void) {
    RUN_TEST(test_the_sine_is_exact_at_the_quarter_points);
    RUN_TEST(test_the_sine_is_within_one_unit_of_the_true_sine_over_two_turns);
    RUN_TEST(test_the_sine_is_odd_and_periodic_for_negative_angles);
    RUN_TEST(test_a_quarter_turn_about_y_is_an_exact_rotation);
    RUN_TEST(test_a_point_transform_rounds_ties_away_from_zero);
    RUN_TEST(test_a_matrix_product_rounds_to_nearest);
}

SUITE_REGISTER(run_matrix4i_suite);
