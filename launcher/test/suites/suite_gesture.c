/*
 * The home gesture: a swipe from the edge it is told (gesture_edge_t, picked
 * by shell_exit_edge_for_quarter()) toward the centre. It must trigger with a
 * fingertip yet never fire in normal use. Boundaries are pinned on the bottom
 * edge, whose logic the others share; the others prove axis and sign.
 */

#include "input/gesture.h"
#include "suites.h"
#include "unity.h"

#define SCREEN_W 368
#define SCREEN_H 448

/* Builds the input state for a finger currently at (x, y) that first touched
 * down at (press_x, press_y). */
static input_t
dragging(int press_x, int press_y, int x, int y) {
    input_t in = {0};
    in.down = true;
    in.press_x = press_x;
    in.press_y = press_y;
    in.x = x;
    in.y = y;
    return in;
}

/* bottom edge: what should trigger it */

void
test_swipe_up_from_the_bottom_edge_triggers(void) {
    /* Starts 20 px from the bottom, travels 150 px up. */
    input_t in = dragging(180, SCREEN_H - 20, 180, SCREEN_H - 170);
    TEST_ASSERT_TRUE(gesture_is_home_swipe(&in, GESTURE_EDGE_BOTTOM, SCREEN_W, SCREEN_H));
}

void
test_it_triggers_anywhere_along_the_bottom_edge(void) {
    /* No target to miss is the entire point of using a gesture. */
    const int xs[] = {5, 100, 184, 300, 363};
    for (unsigned i = 0; i < sizeof(xs) / sizeof(xs[0]); i++) {
        input_t in = dragging(xs[i], SCREEN_H - 10, xs[i], SCREEN_H - 160);
        TEST_ASSERT_TRUE_MESSAGE(gesture_is_home_swipe(&in, GESTURE_EDGE_BOTTOM, SCREEN_W, SCREEN_H),
                                 "a swipe from the bottom edge must work at any x");
    }
}

void
test_a_diagonal_swipe_still_counts(void) {
    /* Fingers do not travel in straight lines; only vertical travel matters. */
    input_t in = dragging(100, SCREEN_H - 20, 260, SCREEN_H - 170);
    TEST_ASSERT_TRUE(gesture_is_home_swipe(&in, GESTURE_EDGE_BOTTOM, SCREEN_W, SCREEN_H));
}

/* bottom edge: what should not */

void
test_a_swipe_starting_mid_screen_does_not_trigger(void) {
    /* Otherwise any upward drag inside an app would close it. */
    input_t in = dragging(180, SCREEN_H / 2, 180, SCREEN_H / 2 - 200);
    TEST_ASSERT_FALSE(gesture_is_home_swipe(&in, GESTURE_EDGE_BOTTOM, SCREEN_W, SCREEN_H));
}

void
test_a_short_swipe_does_not_trigger(void) {
    /* Guards against a tap near the bottom edge wobbling a few pixels. */
    input_t in = dragging(180, SCREEN_H - 10, 180, SCREEN_H - 40);
    TEST_ASSERT_FALSE(gesture_is_home_swipe(&in, GESTURE_EDGE_BOTTOM, SCREEN_W, SCREEN_H));
}

void
test_a_downward_swipe_does_not_trigger(void) {
    input_t in = dragging(180, SCREEN_H - 200, 180, SCREEN_H - 20);
    TEST_ASSERT_FALSE(gesture_is_home_swipe(&in, GESTURE_EDGE_BOTTOM, SCREEN_W, SCREEN_H));
}

void
test_a_lifted_finger_does_not_trigger(void) {
    /* The gesture fires mid-swipe, so it must require contact; otherwise the
     * stale coordinates left behind after a lift would re-trigger it. */
    input_t in = dragging(180, SCREEN_H - 20, 180, SCREEN_H - 170);
    in.down = false;
    TEST_ASSERT_FALSE(gesture_is_home_swipe(&in, GESTURE_EDGE_BOTTOM, SCREEN_W, SCREEN_H));
}

void
test_a_stationary_press_at_the_bottom_does_not_trigger(void) {
    input_t in = dragging(180, SCREEN_H - 10, 180, SCREEN_H - 10);
    TEST_ASSERT_FALSE(gesture_is_home_swipe(&in, GESTURE_EDGE_BOTTOM, SCREEN_W, SCREEN_H));
}

/* bottom edge: boundaries */

void
test_the_start_zone_boundary_is_inclusive(void) {
    const int edge = SCREEN_H - GESTURE_HOME_ZONE_DEPTH;

    input_t just_inside = dragging(180, edge, 180, edge - GESTURE_HOME_SWIPE_DIST);
    TEST_ASSERT_TRUE_MESSAGE(gesture_is_home_swipe(&just_inside, GESTURE_EDGE_BOTTOM, SCREEN_W, SCREEN_H),
                             "starting exactly at the zone boundary should count");

    input_t just_outside = dragging(180, edge - 1, 180, edge - 1 - GESTURE_HOME_SWIPE_DIST);
    TEST_ASSERT_FALSE_MESSAGE(gesture_is_home_swipe(&just_outside, GESTURE_EDGE_BOTTOM, SCREEN_W, SCREEN_H),
                              "starting one pixel above the zone should not");
}

void
test_the_distance_threshold_is_inclusive(void) {
    const int start = SCREEN_H - 10;

    input_t exactly = dragging(180, start, 180, start - GESTURE_HOME_SWIPE_DIST);
    TEST_ASSERT_TRUE_MESSAGE(gesture_is_home_swipe(&exactly, GESTURE_EDGE_BOTTOM, SCREEN_W, SCREEN_H),
                             "travelling exactly the threshold should count");

    input_t one_short = dragging(180, start, 180, start - GESTURE_HOME_SWIPE_DIST + 1);
    TEST_ASSERT_FALSE_MESSAGE(gesture_is_home_swipe(&one_short, GESTURE_EDGE_BOTTOM, SCREEN_W, SCREEN_H),
                              "one pixel short should not");
}

/* top edge */

void
test_swipe_down_from_the_top_edge_triggers(void) {
    /* Starts 20 px from the top, travels 150 px down, the mirror image of
     * the bottom-edge case. */
    input_t in = dragging(180, 20, 180, 170);
    TEST_ASSERT_TRUE(gesture_is_home_swipe(&in, GESTURE_EDGE_TOP, SCREEN_W, SCREEN_H));
}

void
test_an_upward_swipe_from_the_top_does_not_trigger(void) {
    /* Wrong direction: away from the screen entirely, not toward the centre. */
    input_t in = dragging(180, 20, 180, -130);
    TEST_ASSERT_FALSE(gesture_is_home_swipe(&in, GESTURE_EDGE_TOP, SCREEN_W, SCREEN_H));
}

void
test_a_short_swipe_from_the_top_does_not_trigger(void) {
    input_t in = dragging(180, 10, 180, 40);
    TEST_ASSERT_FALSE(gesture_is_home_swipe(&in, GESTURE_EDGE_TOP, SCREEN_W, SCREEN_H));
}

void
test_a_swipe_starting_mid_screen_does_not_trigger_the_top_edge(void) {
    input_t in = dragging(180, SCREEN_H / 2, 180, SCREEN_H / 2 + 200);
    TEST_ASSERT_FALSE(gesture_is_home_swipe(&in, GESTURE_EDGE_TOP, SCREEN_W, SCREEN_H));
}

/* left edge */

void
test_swipe_right_from_the_left_edge_triggers(void) {
    /* Starts 20 px from the left, travels 150 px right. */
    input_t in = dragging(20, 240, 170, 240);
    TEST_ASSERT_TRUE(gesture_is_home_swipe(&in, GESTURE_EDGE_LEFT, SCREEN_W, SCREEN_H));
}

void
test_a_leftward_swipe_from_the_left_does_not_trigger(void) {
    /* Wrong direction: further off the left edge, not toward the centre. */
    input_t in = dragging(20, 240, -130, 240);
    TEST_ASSERT_FALSE(gesture_is_home_swipe(&in, GESTURE_EDGE_LEFT, SCREEN_W, SCREEN_H));
}

void
test_a_short_swipe_from_the_left_does_not_trigger(void) {
    input_t in = dragging(10, 240, 40, 240);
    TEST_ASSERT_FALSE(gesture_is_home_swipe(&in, GESTURE_EDGE_LEFT, SCREEN_W, SCREEN_H));
}

void
test_a_swipe_starting_mid_screen_does_not_trigger_the_left_edge(void) {
    input_t in = dragging(SCREEN_W / 2, 240, SCREEN_W / 2 + 200, 240);
    TEST_ASSERT_FALSE(gesture_is_home_swipe(&in, GESTURE_EDGE_LEFT, SCREEN_W, SCREEN_H));
}

/* right edge */

void
test_swipe_left_from_the_right_edge_triggers(void) {
    /* Starts 20 px from the right, travels 150 px left. */
    input_t in = dragging(SCREEN_W - 20, 240, SCREEN_W - 170, 240);
    TEST_ASSERT_TRUE(gesture_is_home_swipe(&in, GESTURE_EDGE_RIGHT, SCREEN_W, SCREEN_H));
}

void
test_a_rightward_swipe_from_the_right_does_not_trigger(void) {
    /* Wrong direction: further off the right edge, not toward the centre. */
    input_t in = dragging(SCREEN_W - 20, 240, SCREEN_W + 130, 240);
    TEST_ASSERT_FALSE(gesture_is_home_swipe(&in, GESTURE_EDGE_RIGHT, SCREEN_W, SCREEN_H));
}

void
test_a_short_swipe_from_the_right_does_not_trigger(void) {
    input_t in = dragging(SCREEN_W - 10, 240, SCREEN_W - 40, 240);
    TEST_ASSERT_FALSE(gesture_is_home_swipe(&in, GESTURE_EDGE_RIGHT, SCREEN_W, SCREEN_H));
}

void
test_a_swipe_starting_mid_screen_does_not_trigger_the_right_edge(void) {
    input_t in = dragging(SCREEN_W / 2, 240, SCREEN_W / 2 - 200, 240);
    TEST_ASSERT_FALSE(gesture_is_home_swipe(&in, GESTURE_EDGE_RIGHT, SCREEN_W, SCREEN_H));
}

/* suite */

void
test_each_edge_is_opposite_the_one_across_and_back(void) {
    const gesture_edge_t edges[] = {GESTURE_EDGE_TOP, GESTURE_EDGE_BOTTOM, GESTURE_EDGE_LEFT, GESTURE_EDGE_RIGHT};
    TEST_ASSERT_EQUAL_INT(GESTURE_EDGE_TOP, gesture_opposite_edge(GESTURE_EDGE_BOTTOM));
    TEST_ASSERT_EQUAL_INT(GESTURE_EDGE_RIGHT, gesture_opposite_edge(GESTURE_EDGE_LEFT));
    for (unsigned i = 0; i < sizeof(edges) / sizeof(edges[0]); i++) {
        TEST_ASSERT_NOT_EQUAL_INT(edges[i], gesture_opposite_edge(edges[i]));
        TEST_ASSERT_EQUAL_INT(edges[i], gesture_opposite_edge(gesture_opposite_edge(edges[i])));
    }
}

void
test_the_edge_zone_reaches_its_depth_from_each_edge_and_no_further(void) {
    const int mid_x = SCREEN_W / 2;
    const int mid_y = SCREEN_H / 2;
    const int d = GESTURE_HOME_ZONE_DEPTH;
    TEST_ASSERT_TRUE(gesture_in_edge_zone(mid_x, SCREEN_H - d, GESTURE_EDGE_BOTTOM, SCREEN_W, SCREEN_H));
    TEST_ASSERT_FALSE(gesture_in_edge_zone(mid_x, SCREEN_H - d - 1, GESTURE_EDGE_BOTTOM, SCREEN_W, SCREEN_H));
    TEST_ASSERT_TRUE(gesture_in_edge_zone(mid_x, d, GESTURE_EDGE_TOP, SCREEN_W, SCREEN_H));
    TEST_ASSERT_FALSE(gesture_in_edge_zone(mid_x, d + 1, GESTURE_EDGE_TOP, SCREEN_W, SCREEN_H));
    TEST_ASSERT_TRUE(gesture_in_edge_zone(d, mid_y, GESTURE_EDGE_LEFT, SCREEN_W, SCREEN_H));
    TEST_ASSERT_FALSE(gesture_in_edge_zone(d + 1, mid_y, GESTURE_EDGE_LEFT, SCREEN_W, SCREEN_H));
    TEST_ASSERT_TRUE(gesture_in_edge_zone(SCREEN_W - d, mid_y, GESTURE_EDGE_RIGHT, SCREEN_W, SCREEN_H));
    TEST_ASSERT_FALSE(gesture_in_edge_zone(SCREEN_W - d - 1, mid_y, GESTURE_EDGE_RIGHT, SCREEN_W, SCREEN_H));
}

void
run_gesture_suite(void) {
    RUN_TEST(test_each_edge_is_opposite_the_one_across_and_back);
    RUN_TEST(test_the_edge_zone_reaches_its_depth_from_each_edge_and_no_further);

    RUN_TEST(test_swipe_up_from_the_bottom_edge_triggers);
    RUN_TEST(test_it_triggers_anywhere_along_the_bottom_edge);
    RUN_TEST(test_a_diagonal_swipe_still_counts);

    RUN_TEST(test_a_swipe_starting_mid_screen_does_not_trigger);
    RUN_TEST(test_a_short_swipe_does_not_trigger);
    RUN_TEST(test_a_downward_swipe_does_not_trigger);
    RUN_TEST(test_a_lifted_finger_does_not_trigger);
    RUN_TEST(test_a_stationary_press_at_the_bottom_does_not_trigger);

    RUN_TEST(test_the_start_zone_boundary_is_inclusive);
    RUN_TEST(test_the_distance_threshold_is_inclusive);

    RUN_TEST(test_swipe_down_from_the_top_edge_triggers);
    RUN_TEST(test_an_upward_swipe_from_the_top_does_not_trigger);
    RUN_TEST(test_a_short_swipe_from_the_top_does_not_trigger);
    RUN_TEST(test_a_swipe_starting_mid_screen_does_not_trigger_the_top_edge);

    RUN_TEST(test_swipe_right_from_the_left_edge_triggers);
    RUN_TEST(test_a_leftward_swipe_from_the_left_does_not_trigger);
    RUN_TEST(test_a_short_swipe_from_the_left_does_not_trigger);
    RUN_TEST(test_a_swipe_starting_mid_screen_does_not_trigger_the_left_edge);

    RUN_TEST(test_swipe_left_from_the_right_edge_triggers);
    RUN_TEST(test_a_rightward_swipe_from_the_right_does_not_trigger);
    RUN_TEST(test_a_short_swipe_from_the_right_does_not_trigger);
    RUN_TEST(test_a_swipe_starting_mid_screen_does_not_trigger_the_right_edge);
}

SUITE_REGISTER(run_gesture_suite);
