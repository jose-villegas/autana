/*
 * Portable suite: util/mat4i.h. The sine and the rounding of its products,
 * judged against the exact value rather than against a pinned render.
 */

#include "suites.h"
#include "unity.h"

#include <math.h>

#include "util/mat4i.h"

static void
test_the_sine_is_exact_at_the_quarter_points(void) {
    TEST_ASSERT_EQUAL_INT(0, m4_sin(0));
    TEST_ASSERT_EQUAL_INT(M4_ONE, m4_sin(M4_ONE / 4));
    TEST_ASSERT_EQUAL_INT(0, m4_sin(M4_ONE / 2));
    TEST_ASSERT_EQUAL_INT(-M4_ONE, m4_sin(3 * M4_ONE / 4));
    TEST_ASSERT_EQUAL_INT(M4_ONE, m4_cos(0));
}

static void
test_the_sine_is_within_one_unit_of_the_true_sine_over_two_turns(void) {
    for (int angle = -M4_ONE; angle < M4_ONE; angle++) {
        const double exact = sin(2.0 * 3.14159265358979323846 * angle / M4_ONE) * M4_ONE;
        TEST_ASSERT_TRUE_MESSAGE(fabs(m4_sin(angle) - exact) <= 1.0, "sine off by more than one unit");
    }
}

static void
test_the_sine_is_odd_and_periodic_for_negative_angles(void) {
    for (int angle = 1; angle < M4_ONE; angle++) {
        TEST_ASSERT_EQUAL_INT(-m4_sin(angle), m4_sin(-angle));
        TEST_ASSERT_EQUAL_INT(m4_sin(angle), m4_sin(angle - M4_ONE));
        TEST_ASSERT_EQUAL_INT(m4_sin(M4_ONE / 2 - angle), m4_sin(angle));
    }
}

static void
test_a_quarter_turn_about_y_is_an_exact_rotation(void) {
    m4_mat_t m;
    m4_rotation_matrix(0, M4_ONE / 4, 0, m);
    TEST_ASSERT_EQUAL_INT(0, m[0][0]);
    TEST_ASSERT_EQUAL_INT(0, m[2][2]);
    TEST_ASSERT_EQUAL_INT(M4_ONE, m[2][0]);
    TEST_ASSERT_EQUAL_INT(-M4_ONE, m[0][2]);
    TEST_ASSERT_EQUAL_INT(M4_ONE, m[1][1]);
}

static void
test_a_point_transform_rounds_ties_away_from_zero(void) {
    m4_mat_t half;
    m4_scale_matrix(M4_ONE / 2, M4_ONE / 2, M4_ONE / 2, half);
    m4_vec4_t point = {3, -3, 5, M4_ONE};
    m4_vec3_transform(&point, half);
    TEST_ASSERT_EQUAL_INT(2, point.x);
    TEST_ASSERT_EQUAL_INT(-2, point.y);
    TEST_ASSERT_EQUAL_INT(3, point.z);
}

static void
test_a_matrix_product_rounds_to_nearest(void) {
    m4_mat_t half, other;
    m4_scale_matrix(M4_ONE / 2, M4_ONE / 2, M4_ONE / 2, half);
    m4_scale_matrix(3, 3, 3, other);
    m4_mat_mul(half, other);
    TEST_ASSERT_EQUAL_INT(2, half[0][0]);
}

void
run_mat4i_suite(void) {
    RUN_TEST(test_the_sine_is_exact_at_the_quarter_points);
    RUN_TEST(test_the_sine_is_within_one_unit_of_the_true_sine_over_two_turns);
    RUN_TEST(test_the_sine_is_odd_and_periodic_for_negative_angles);
    RUN_TEST(test_a_quarter_turn_about_y_is_an_exact_rotation);
    RUN_TEST(test_a_point_transform_rounds_ties_away_from_zero);
    RUN_TEST(test_a_matrix_product_rounds_to_nearest);
}

SUITE_REGISTER(run_mat4i_suite);
