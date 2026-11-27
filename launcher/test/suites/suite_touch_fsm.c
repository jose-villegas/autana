/* Touch samples and their timestamps drive the press and release edge tests. */

#include "input/touch/touch_fsm.h"
#include "suites.h"
#include "unity.h"

#include "../input_examples/touch.c"

/* Time is supplied, never read from a clock, so tests are instant and
 * deterministic. All helpers work in milliseconds for readability. */
#define MS(x) ((int64_t)(x) * 1000)

static touch_fsm_t fsm;

/* Each test starts from a clean state. Unity's setUp() is global and this
 * suite shares a binary with others, so the fixture is explicit instead. */
static void
fixture(void) {
    touch_fsm_init(&fsm);
}

/* pressing */

void
test_starts_with_nothing_pressed(void) {
    fixture();
    input_t in;
    touch_fsm_take(&fsm, &in);

    TEST_ASSERT_FALSE(in.down);
    TEST_ASSERT_FALSE(in.pressed);
    TEST_ASSERT_FALSE(in.released);
}

void
test_first_contact_reports_a_press(void) {
    fixture();
    touch_fsm_update(&fsm, true, false, (vec2i_t){100, 200}, MS(0));

    input_t in;
    touch_fsm_take(&fsm, &in);

    TEST_ASSERT_TRUE(in.pressed);
    TEST_ASSERT_TRUE(in.down);
    TEST_ASSERT_EQUAL_INT(100, in.point.x);
    TEST_ASSERT_EQUAL_INT(200, in.point.y);
}

void
test_press_is_reported_only_once(void) {
    fixture();
    touch_fsm_update(&fsm, true, false, (vec2i_t){100, 200}, MS(0));

    input_t first, second;
    touch_fsm_take(&fsm, &first);
    touch_fsm_take(&fsm, &second);

    TEST_ASSERT_TRUE(first.pressed);
    TEST_ASSERT_FALSE_MESSAGE(second.pressed, "an edge must be consumed by the frame that reads it");
}

void
test_holding_does_not_repeat_the_press(void) {
    fixture();
    touch_fsm_update(&fsm, true, false, (vec2i_t){100, 200}, MS(0));
    input_t in;
    touch_fsm_take(&fsm, &in);

    touch_fsm_update(&fsm, true, false, (vec2i_t){100, 200}, MS(10));
    touch_fsm_take(&fsm, &in);

    TEST_ASSERT_FALSE(in.pressed);
    TEST_ASSERT_TRUE_MESSAGE(in.down, "the finger is still down");
}

void
test_press_position_is_where_the_touch_began(void) {
    fixture();
    touch_fsm_update(&fsm, true, false, (vec2i_t){50, 400}, MS(0));
    touch_fsm_update(&fsm, true, false, (vec2i_t){55, 300}, MS(20));
    touch_fsm_update(&fsm, true, false, (vec2i_t){60, 200}, MS(40));

    input_t in;
    touch_fsm_take(&fsm, &in);

    TEST_ASSERT_EQUAL_INT_MESSAGE(50, in.press.x, "press_x must not follow the finger");
    TEST_ASSERT_EQUAL_INT_MESSAGE(400, in.press.y, "press_y must not follow the finger");
    TEST_ASSERT_EQUAL_INT_MESSAGE(60, in.point.x, "x tracks the current position");
    TEST_ASSERT_EQUAL_INT_MESSAGE(200, in.point.y, "y tracks the current position");
}

/* releasing, and the debounce that makes it reliable */

static void
test_lift_releases_on_the_same_sample(void) {
    fixture();
    touch_fsm_update(&fsm, true, false, vec2i_one(), MS(0));
    input_t in;
    touch_fsm_take(&fsm, &in);

    touch_fsm_update(&fsm, false, true, vec2i_zero(), MS(10));
    touch_fsm_take(&fsm, &in);

    TEST_ASSERT_TRUE(in.released);
    TEST_ASSERT_FALSE(in.down);
}

static void
test_contact_after_a_lift_presses_again(void) {
    fixture();
    touch_fsm_update(&fsm, true, false, vec2i_one(), MS(0));
    input_t in;
    touch_fsm_take(&fsm, &in);

    touch_fsm_update(&fsm, false, true, vec2i_zero(), MS(10));
    touch_fsm_take(&fsm, &in);
    touch_fsm_update(&fsm, true, false, vec2i_one(), MS(20));
    touch_fsm_take(&fsm, &in);

    TEST_ASSERT_TRUE(in.pressed);
    TEST_ASSERT_TRUE(in.down);
    TEST_ASSERT_FALSE(in.released);
}

static void
test_contact_waits_for_the_latched_release(void) {
    fixture();
    const vec2i_t old_point = {100, 200};
    const vec2i_t new_point = {50, 300};
    input_t in;
    touch_fsm_update(&fsm, true, false, old_point, MS(0));
    touch_fsm_take(&fsm, &in);

    touch_fsm_update(&fsm, false, true, vec2i_zero(), MS(10));
    touch_fsm_update(&fsm, true, false, new_point, MS(20));
    touch_fsm_take(&fsm, &in);

    TEST_ASSERT_TRUE(in.released);
    TEST_ASSERT_FALSE(in.pressed);
    TEST_ASSERT_FALSE(in.down);
    TEST_ASSERT_TRUE_MESSAGE(vec2i_equal(old_point, in.press), "release retains the old press position");
    TEST_ASSERT_TRUE_MESSAGE(vec2i_equal(old_point, in.point), "release retains the old contact position");

    touch_fsm_update(&fsm, true, false, new_point, MS(30));
    touch_fsm_take(&fsm, &in);

    TEST_ASSERT_TRUE(in.pressed);
    TEST_ASSERT_TRUE(in.down);
    TEST_ASSERT_FALSE(in.released);
    TEST_ASSERT_TRUE(vec2i_equal(new_point, in.press));
    TEST_ASSERT_TRUE(vec2i_equal(new_point, in.point));
}

static void
test_lift_while_idle_produces_no_edge(void) {
    fixture();
    touch_fsm_update(&fsm, false, true, vec2i_zero(), MS(0));
    input_t in;
    touch_fsm_take(&fsm, &in);

    TEST_ASSERT_FALSE(in.down);
    TEST_ASSERT_FALSE(in.pressed);
    TEST_ASSERT_FALSE(in.released);
}

static void
test_contact_with_lift_flag_keeps_the_touch_down(void) {
    fixture();
    touch_fsm_update(&fsm, true, false, vec2i_one(), MS(0));
    input_t in;
    touch_fsm_take(&fsm, &in);

    const vec2i_t moved = vec2i_add(vec2i_one(), vec2i_one());
    touch_fsm_update(&fsm, true, true, moved, MS(10));
    touch_fsm_take(&fsm, &in);

    TEST_ASSERT_TRUE(in.down);
    TEST_ASSERT_FALSE(in.pressed);
    TEST_ASSERT_FALSE(in.released);
    TEST_ASSERT_EQUAL_INT(moved.x, in.point.x);
    TEST_ASSERT_EQUAL_INT(moved.y, in.point.y);
}

void
test_release_is_reported_after_the_quiet_period(void) {
    fixture();
    touch_fsm_update(&fsm, true, false, (vec2i_t){10, 10}, MS(0));
    input_t in;
    touch_fsm_take(&fsm, &in);

    touch_fsm_update(&fsm, false, false, vec2i_zero(), MS(0) + TOUCH_RELEASE_QUIET_US + 1);
    touch_fsm_take(&fsm, &in);

    TEST_ASSERT_TRUE(in.released);
    TEST_ASSERT_FALSE(in.down);
}

void
test_brief_dropout_is_not_a_release(void) {
    fixture();
    touch_fsm_update(&fsm, true, false, (vec2i_t){10, 10}, MS(0));
    input_t in;
    touch_fsm_take(&fsm, &in);

    touch_fsm_update(&fsm, false, false, vec2i_zero(), MS(10));
    touch_fsm_take(&fsm, &in);

    TEST_ASSERT_FALSE_MESSAGE(in.released, "a short gap is not a release");
    TEST_ASSERT_TRUE_MESSAGE(in.down, "the finger is still considered down");
}

void
test_contact_resuming_after_a_dropout_does_not_re_press(void) {
    fixture();
    touch_fsm_update(&fsm, true, false, (vec2i_t){10, 10}, MS(0));
    input_t in;
    touch_fsm_take(&fsm, &in);

    touch_fsm_update(&fsm, false, false, vec2i_zero(), MS(10));     /* dropout */
    touch_fsm_update(&fsm, true, false, (vec2i_t){12, 12}, MS(20)); /* back again */
    touch_fsm_take(&fsm, &in);

    TEST_ASSERT_FALSE_MESSAGE(in.pressed, "a dropout mid-touch must not produce a second press");
    TEST_ASSERT_FALSE(in.released);
    TEST_ASSERT_TRUE(in.down);
}

void
test_quiet_period_is_measured_from_the_last_contact(void) {
    fixture();
    /* Contact keeps arriving, each time restarting the clock, so the total
     * elapsed time far exceeds the quiet period without ever releasing. */
    touch_fsm_update(&fsm, true, false, (vec2i_t){10, 10}, MS(0));
    input_t in;
    touch_fsm_take(&fsm, &in);

    for (int t = 10; t <= 500; t += 10) {
        touch_fsm_update(&fsm, true, false, (vec2i_t){10, 10}, MS(t));
    }
    touch_fsm_take(&fsm, &in);

    TEST_ASSERT_FALSE(in.released);
    TEST_ASSERT_TRUE(in.down);
}

void
test_a_full_tap_produces_exactly_one_press_and_one_release(void) {
    fixture();
    int presses = 0, releases = 0;
    input_t in;

    touch_fsm_update(&fsm, true, false, (vec2i_t){80, 90}, MS(0));
    touch_fsm_update(&fsm, true, false, (vec2i_t){80, 90}, MS(20));
    for (int t = 40; t <= 400; t += 10) {
        touch_fsm_update(&fsm, false, false, vec2i_zero(), MS(t));
    }

    /* Drain everything the sequence produced. */
    for (int i = 0; i < 8; i++) {
        touch_fsm_take(&fsm, &in);
        if (in.pressed) {
            presses++;
        }
        if (in.released) {
            releases++;
        }
    }

    TEST_ASSERT_EQUAL_INT(1, presses);
    TEST_ASSERT_EQUAL_INT(1, releases);
}

static void
test_touch_example_counts_edges_and_tracks_held_position(void) {
    fixture();
    const vec2i_t start = vec2i_one();
    const vec2i_t middle = vec2i_add(start, start);
    const vec2i_t last = vec2i_add(middle, start);
    const int64_t step_us = TOUCH_RELEASE_QUIET_US;
    touch_example_t state = {0};
    input_t input;
    touch_fsm_update(&fsm, true, false, start, 0);
    touch_fsm_take(&fsm, &input);
    touch_example(&state, &input);
    touch_fsm_update(&fsm, true, false, middle, step_us);
    touch_fsm_take(&fsm, &input);
    touch_example(&state, &input);
    touch_fsm_update(&fsm, true, false, last, step_us + step_us);
    touch_fsm_take(&fsm, &input);
    touch_example(&state, &input);
    touch_fsm_update(&fsm, false, false, start, step_us + step_us + TOUCH_RELEASE_QUIET_US + 1);
    touch_fsm_take(&fsm, &input);
    TEST_ASSERT_TRUE(input.released);
    /* Distinct release coordinates prove the example follows only while down. */
    input.point = start;
    touch_example(&state, &input);
    TEST_ASSERT_EQUAL_INT(1, state.taps);
    TEST_ASSERT_EQUAL_INT(last.x, state.point.x);
    TEST_ASSERT_EQUAL_INT(last.y, state.point.y);
    touch_fsm_update(&fsm, true, false, start, step_us * 4);
    touch_fsm_take(&fsm, &input);
    TEST_ASSERT_TRUE(input.pressed);
    touch_example(&state, &input);
    TEST_ASSERT_EQUAL_INT(2, state.taps);
    TEST_ASSERT_EQUAL_INT(start.x, state.point.x);
    TEST_ASSERT_EQUAL_INT(start.y, state.point.y);
}

/* suite */

void
run_touch_fsm_suite(void) {

    RUN_TEST(test_touch_example_counts_edges_and_tracks_held_position);
    RUN_TEST(test_starts_with_nothing_pressed);
    RUN_TEST(test_first_contact_reports_a_press);
    RUN_TEST(test_press_is_reported_only_once);
    RUN_TEST(test_holding_does_not_repeat_the_press);
    RUN_TEST(test_press_position_is_where_the_touch_began);

    RUN_TEST(test_lift_releases_on_the_same_sample);
    RUN_TEST(test_contact_after_a_lift_presses_again);
    RUN_TEST(test_contact_waits_for_the_latched_release);
    RUN_TEST(test_lift_while_idle_produces_no_edge);
    RUN_TEST(test_contact_with_lift_flag_keeps_the_touch_down);
    RUN_TEST(test_release_is_reported_after_the_quiet_period);
    RUN_TEST(test_brief_dropout_is_not_a_release);
    RUN_TEST(test_contact_resuming_after_a_dropout_does_not_re_press);
    RUN_TEST(test_quiet_period_is_measured_from_the_last_contact);
    RUN_TEST(test_a_full_tap_produces_exactly_one_press_and_one_release);
}

SUITE_REGISTER(run_touch_fsm_suite);
