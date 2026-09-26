/*
 * Portable suite: corner_arc - the inset a rounded corner hides on each row.
 */

#include "suites.h"
#include "unity.h"

#include "apps/input_lab/corner_arc.h"

static void
test_the_first_row_is_hidden_across_the_whole_radius(void) {
    TEST_ASSERT_EQUAL_INT(40, corner_arc_inset(40, 0));
}

static void
test_rows_past_the_radius_hide_nothing(void) {
    TEST_ASSERT_EQUAL_INT(0, corner_arc_inset(40, 40));
    TEST_ASSERT_EQUAL_INT(0, corner_arc_inset(40, 200));
}

/* Halfway down a radius-40 corner the circle is 40 - sqrt(40^2 - 20^2) =
 * 5.36 in from the edge, which a pixel rounds to 5. */
static void
test_halfway_down_the_inset_follows_the_circle(void) {
    TEST_ASSERT_EQUAL_INT(5, corner_arc_inset(40, 20));
}

static void
test_the_inset_never_grows_down_the_corner(void) {
    for (int row = 1; row < 60; row++) {
        TEST_ASSERT_LESS_OR_EQUAL_INT(corner_arc_inset(60, row - 1), corner_arc_inset(60, row));
    }
}

static void
test_a_square_corner_hides_nothing(void) {
    TEST_ASSERT_EQUAL_INT(0, corner_arc_inset(0, 0));
}

void
run_corner_arc_suite(void) {
    RUN_TEST(test_the_first_row_is_hidden_across_the_whole_radius);
    RUN_TEST(test_rows_past_the_radius_hide_nothing);
    RUN_TEST(test_halfway_down_the_inset_follows_the_circle);
    RUN_TEST(test_the_inset_never_grows_down_the_corner);
    RUN_TEST(test_a_square_corner_hides_nothing);
}

SUITE_REGISTER(run_corner_arc_suite);
