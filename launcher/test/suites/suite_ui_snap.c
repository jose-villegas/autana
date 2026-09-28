#include "suites.h"
#include "unity.h"

#include "ui/ui_snap.h"

static void
assert_snaps_to(const mu_Rect* rects, int count, int x, int y, int reach, int expected_x, int expected_y) {
    const mu_Vec2 snapped = ui_snap_point(rects, count, mu_vec2(x, y), reach);
    TEST_ASSERT_EQUAL_INT(expected_x, snapped.x);
    TEST_ASSERT_EQUAL_INT(expected_y, snapped.y);
}

static void
test_inside_a_control_is_unchanged(void) {
    const mu_Rect rect = {40, 50, 20, 30};
    assert_snaps_to(&rect, 1, 45, 60, 40, 45, 60);
}

static void
test_just_outside_snaps_to_the_nearest_edge(void) {
    const mu_Rect rect = {40, 50, 20, 30};
    assert_snaps_to(&rect, 1, 35, 60, 40, 40, 60);
}

static void
test_a_point_beyond_reach_is_unchanged(void) {
    const mu_Rect rect = {40, 50, 20, 30};
    assert_snaps_to(&rect, 1, 0, 60, 39, 0, 60);
}

static void
test_nearest_candidate_wins_and_a_tie_keeps_order(void) {
    const mu_Rect rects[] = {{40, 40, 11, 10}, {60, 40, 10, 10}};
    assert_snaps_to(rects, 2, 55, 45, 20, 50, 45);
    assert_snaps_to(rects, 2, 53, 45, 20, 50, 45);
}

static void
test_an_empty_list_is_unchanged(void) {
    assert_snaps_to(NULL, 0, 10, 20, 40, 10, 20);
}

void
run_ui_snap_suite(void) {
    RUN_TEST(test_inside_a_control_is_unchanged);
    RUN_TEST(test_just_outside_snaps_to_the_nearest_edge);
    RUN_TEST(test_a_point_beyond_reach_is_unchanged);
    RUN_TEST(test_nearest_candidate_wins_and_a_tie_keeps_order);
    RUN_TEST(test_an_empty_list_is_unchanged);
}

SUITE_REGISTER(run_ui_snap_suite);
