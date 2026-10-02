/*
 * Portable suite: touch_gesture_name, the console's word for a finished
 * gesture.
 */

#include "suites.h"
#include "unity.h"

#include "input/touch_gesture.h"

static void
test_each_gesture_has_its_console_word(void) {
    TEST_ASSERT_EQUAL_STRING("TAP", touch_gesture_name(TOUCH_GESTURE_TAP));
    TEST_ASSERT_EQUAL_STRING("PRESS", touch_gesture_name(TOUCH_GESTURE_PRESS));
    TEST_ASSERT_EQUAL_STRING("DRAG", touch_gesture_name(TOUCH_GESTURE_DRAG));
}

static void
test_no_gesture_has_no_word(void) {
    TEST_ASSERT_NULL(touch_gesture_name(TOUCH_GESTURE_NONE));
}

void
run_touch_gesture_name_suite(void) {
    RUN_TEST(test_each_gesture_has_its_console_word);
    RUN_TEST(test_no_gesture_has_no_word);
}

SUITE_REGISTER(run_touch_gesture_name_suite);
