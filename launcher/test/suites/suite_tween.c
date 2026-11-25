/* Portable timeline and easing contracts, independent of animation state. */

#include <stdint.h>

#include "suites.h"
#include "unity.h"

#include "math/scalar/ease.h"

/* ease_ramp_u8() */

static void
test_a_ramp_is_flat_before_and_after(void) {
    TEST_ASSERT_EQUAL_UINT8(0, ease_ramp_u8(0, 100, 200));
    TEST_ASSERT_EQUAL_UINT8(0, ease_ramp_u8(100, 100, 200));
    TEST_ASSERT_EQUAL_UINT8(255, ease_ramp_u8(300, 100, 200));
    TEST_ASSERT_EQUAL_UINT8(255, ease_ramp_u8(9999, 100, 200));
}

static void
test_a_ramp_never_goes_backwards(void) {
    uint8_t last = 0;
    for (uint32_t t = 0; t <= 400; t += 5) {
        const uint8_t v = ease_ramp_u8(t, 100, 200);
        TEST_ASSERT_TRUE_MESSAGE(v >= last, "a ramp must never step backwards as time moves forward");
        last = v;
    }
}

static void
test_a_zero_duration_ramp_jumps_straight_to_the_end(void) {
    TEST_ASSERT_EQUAL_UINT8(0, ease_ramp_u8(100, 100, 0));
    TEST_ASSERT_EQUAL_UINT8(255, ease_ramp_u8(101, 100, 0));
}

/* ease_out_u8() */

static void
test_the_ease_keeps_its_endpoints_and_leads_in_the_middle(void) {
    TEST_ASSERT_EQUAL_UINT8(0, ease_out_u8(0));
    TEST_ASSERT_EQUAL_UINT8(255, ease_out_u8(255));
    TEST_ASSERT_TRUE_MESSAGE(ease_out_u8(128) > 128, "an ease-out is ahead of linear part way through, not behind it");
}

static void
test_the_ease_never_goes_backwards(void) {
    uint8_t last = 0;
    for (int v = 0; v <= 255; v++) {
        const uint8_t eased = ease_out_u8((uint8_t)v);
        TEST_ASSERT_TRUE_MESSAGE(eased >= last, "easing a monotonic input must stay monotonic");
        last = eased;
    }
}

/* ease_in_u8() */

static void
test_the_ease_in_keeps_its_endpoints_and_lags_in_the_middle(void) {
    TEST_ASSERT_EQUAL_UINT8(0, ease_in_u8(0));
    TEST_ASSERT_EQUAL_UINT8(255, ease_in_u8(255));
    TEST_ASSERT_TRUE_MESSAGE(ease_in_u8(128) < 128, "an ease-in is behind linear part way through, not ahead of it");
}

static void
test_the_ease_in_never_goes_backwards(void) {
    uint8_t last = 0;
    for (int v = 0; v <= 255; v++) {
        const uint8_t eased = ease_in_u8((uint8_t)v);
        TEST_ASSERT_TRUE_MESSAGE(eased >= last, "easing a monotonic input must stay monotonic");
        last = eased;
    }
}

/* The whole reason ease_in_u8() exists: composed after ease_out_u8()
 * meets its own end, the two should leave a smooth apex, not a corner, so
 * both should be moving slowly (small steps) right at that shared point. */
static void
test_ease_out_into_ease_in_makes_a_smooth_apex(void) {
    const uint8_t near_end = ease_out_u8(250);
    const uint8_t at_end = ease_out_u8(255);
    const uint8_t at_start = ease_in_u8(0);
    const uint8_t just_after = ease_in_u8(5);

    TEST_ASSERT_EQUAL_UINT8(255, at_end);
    TEST_ASSERT_EQUAL_UINT8(0, at_start);
    TEST_ASSERT_TRUE_MESSAGE((at_end - near_end) <= 3,
                             "ease-out should already have nearly stopped moving by the apex");
    TEST_ASSERT_TRUE_MESSAGE((just_after - at_start) <= 3, "ease-in should still barely be moving just past the apex");
}

static void
test_the_smooth_ease_is_gentle_at_both_ends_and_half_way_at_half_time(void) {
    TEST_ASSERT_EQUAL_INT(0, ease_smooth_q8(0, 4000));
    TEST_ASSERT_EQUAL_INT(128, ease_smooth_q8(2000, 4000));
    TEST_ASSERT_EQUAL_INT(256, ease_smooth_q8(4000, 4000));
    TEST_ASSERT_EQUAL_INT(256, ease_smooth_q8(9000, 4000));
    TEST_ASSERT_EQUAL_INT(256, ease_smooth_q8(0, 0));

    const int first_tenth = ease_smooth_q8(400, 4000);
    const int middle_tenth = ease_smooth_q8(2200, 4000) - ease_smooth_q8(1800, 4000);
    const int last_tenth = 256 - ease_smooth_q8(3600, 4000);
    TEST_ASSERT_TRUE(first_tenth * 3 < middle_tenth);
    TEST_ASSERT_TRUE(last_tenth * 3 < middle_tenth);

    for (uint32_t ms = 1; ms <= 4000; ms++) {
        TEST_ASSERT_TRUE(ease_smooth_q8(ms, 4000) >= ease_smooth_q8(ms - 1, 4000));
    }
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

    RUN_TEST(test_the_smooth_ease_is_gentle_at_both_ends_and_half_way_at_half_time);
}

SUITE_REGISTER(suite_tween);
