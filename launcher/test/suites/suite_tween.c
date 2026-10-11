/* Portable timeline and easing contracts, independent of animation state. */

#include <stdint.h>

#include "suites.h"
#include "unity.h"

#include "anim/tween.h"

/* tween_ramp() */

static void
test_a_ramp_is_flat_before_and_after(void) {
    TEST_ASSERT_EQUAL_UINT8(0, tween_ramp(0, 100, 200));
    TEST_ASSERT_EQUAL_UINT8(0, tween_ramp(100, 100, 200));
    TEST_ASSERT_EQUAL_UINT8(255, tween_ramp(300, 100, 200));
    TEST_ASSERT_EQUAL_UINT8(255, tween_ramp(9999, 100, 200));
}

static void
test_a_ramp_never_goes_backwards(void) {
    uint8_t last = 0;
    for (uint32_t t = 0; t <= 400; t += 5) {
        const uint8_t v = tween_ramp(t, 100, 200);
        TEST_ASSERT_TRUE_MESSAGE(v >= last, "a ramp must never step backwards as time moves forward");
        last = v;
    }
}

static void
test_a_zero_duration_ramp_jumps_straight_to_the_end(void) {
    TEST_ASSERT_EQUAL_UINT8(0, tween_ramp(100, 100, 0));
    TEST_ASSERT_EQUAL_UINT8(255, tween_ramp(101, 100, 0));
}

/* tween_ease_out() */

static void
test_the_ease_keeps_its_endpoints_and_leads_in_the_middle(void) {
    TEST_ASSERT_EQUAL_UINT8(0, tween_ease_out(0));
    TEST_ASSERT_EQUAL_UINT8(255, tween_ease_out(255));
    TEST_ASSERT_TRUE_MESSAGE(tween_ease_out(128) > 128,
                             "an ease-out is ahead of linear part way through, not behind it");
}

static void
test_the_ease_never_goes_backwards(void) {
    uint8_t last = 0;
    for (int v = 0; v <= 255; v++) {
        const uint8_t eased = tween_ease_out((uint8_t)v);
        TEST_ASSERT_TRUE_MESSAGE(eased >= last, "easing a monotonic input must stay monotonic");
        last = eased;
    }
}

/* tween_ease_in() */

static void
test_the_ease_in_keeps_its_endpoints_and_lags_in_the_middle(void) {
    TEST_ASSERT_EQUAL_UINT8(0, tween_ease_in(0));
    TEST_ASSERT_EQUAL_UINT8(255, tween_ease_in(255));
    TEST_ASSERT_TRUE_MESSAGE(tween_ease_in(128) < 128, "an ease-in is behind linear part way through, not ahead of it");
}

static void
test_the_ease_in_never_goes_backwards(void) {
    uint8_t last = 0;
    for (int v = 0; v <= 255; v++) {
        const uint8_t eased = tween_ease_in((uint8_t)v);
        TEST_ASSERT_TRUE_MESSAGE(eased >= last, "easing a monotonic input must stay monotonic");
        last = eased;
    }
}

/* The whole reason tween_ease_in() exists: composed after tween_ease_out()
 * meets its own end, the two should leave a smooth apex, not a corner, so
 * both should be moving slowly (small steps) right at that shared point. */
static void
test_ease_out_into_ease_in_makes_a_smooth_apex(void) {
    const uint8_t near_end = tween_ease_out(250);
    const uint8_t at_end = tween_ease_out(255);
    const uint8_t at_start = tween_ease_in(0);
    const uint8_t just_after = tween_ease_in(5);

    TEST_ASSERT_EQUAL_UINT8(255, at_end);
    TEST_ASSERT_EQUAL_UINT8(0, at_start);
    TEST_ASSERT_TRUE_MESSAGE((at_end - near_end) <= 3,
                             "ease-out should already have nearly stopped moving by the apex");
    TEST_ASSERT_TRUE_MESSAGE((just_after - at_start) <= 3, "ease-in should still barely be moving just past the apex");
}

void
suite_tween(void) {
    RUN_TEST(test_a_ramp_is_flat_before_and_after);
    RUN_TEST(test_a_ramp_never_goes_backwards);
    RUN_TEST(test_a_zero_duration_ramp_jumps_straight_to_the_end);

    RUN_TEST(test_the_ease_keeps_its_endpoints_and_leads_in_the_middle);
    RUN_TEST(test_the_ease_never_goes_backwards);

    RUN_TEST(test_the_ease_in_keeps_its_endpoints_and_lags_in_the_middle);
    RUN_TEST(test_the_ease_in_never_goes_backwards);
    RUN_TEST(test_ease_out_into_ease_in_makes_a_smooth_apex);
}

SUITE_REGISTER(suite_tween);
