/* touch_inject_fsm: scripted contacts driven with a supplied clock. */

#include "input/touch/touch_inject_fsm.h"
#include "suites.h"
#include "unity.h"

static touch_inject_t inject;

static void
fixture(int x0, int y0, int x1, int y1, uint32_t ms) {
    touch_inject_init(&inject, x0, y0, x1, y1, ms);
}

static void
test_a_tap_reports_contact_before_its_lift(void) {
    int x, y;

    fixture(10, 20, 10, 20, 50);
    TEST_ASSERT_TRUE(touch_inject_step(&inject, 100000, &x, &y));
    TEST_ASSERT_EQUAL_INT(10, x);
    TEST_ASSERT_EQUAL_INT(20, y);
    TEST_ASSERT_TRUE(touch_inject_step(&inject, 125000, &x, &y));
    TEST_ASSERT_FALSE(touch_inject_step(&inject, 151000, &x, &y));
}

static void
test_a_drag_reports_intermediate_points_then_its_endpoint(void) {
    int x, y;

    fixture(10, 20, 110, 220, 250);
    TEST_ASSERT_TRUE(touch_inject_step(&inject, 100000, &x, &y));
    TEST_ASSERT_TRUE(touch_inject_step(&inject, 225000, &x, &y));
    TEST_ASSERT_EQUAL_INT(60, x);
    TEST_ASSERT_EQUAL_INT(120, y);
    TEST_ASSERT_TRUE(touch_inject_step(&inject, 350000, &x, &y));
    TEST_ASSERT_EQUAL_INT(110, x);
    TEST_ASSERT_EQUAL_INT(220, y);
    TEST_ASSERT_FALSE(touch_inject_step(&inject, 360000, &x, &y));
}

static void
test_each_completion_has_its_console_word(void) {
    TEST_ASSERT_EQUAL_STRING("TAP", touch_inject_completion_name(TOUCH_INJECT_TAP));
    TEST_ASSERT_EQUAL_STRING("PRESS", touch_inject_completion_name(TOUCH_INJECT_PRESS));
    TEST_ASSERT_EQUAL_STRING("DRAG", touch_inject_completion_name(TOUCH_INJECT_DRAG));
}

static void
test_no_completion_has_no_word(void) {
    TEST_ASSERT_NULL(touch_inject_completion_name(TOUCH_INJECT_NONE));
}

void
suite_touch_inject_fsm(void) {
    RUN_TEST(test_a_tap_reports_contact_before_its_lift);
    RUN_TEST(test_a_drag_reports_intermediate_points_then_its_endpoint);
    RUN_TEST(test_each_completion_has_its_console_word);
    RUN_TEST(test_no_completion_has_no_word);
}

SUITE_REGISTER(suite_touch_inject_fsm)
