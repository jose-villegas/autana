/*
 * Portable suite: touch_probe - where targets may fall, how a tap is scored
 * against one, and the statistics the taps add up to.
 */

#include "suites.h"
#include "unity.h"

#include "apps/input_lab/touch_probe.h"

static void
test_every_target_fits_inside_the_margin(void) {
    uint32_t rng = 1;
    for (int i = 0; i < 5000; i++) {
        const touch_probe_target_t t = touch_probe_next(&rng, 448, 368, 12, 8);
        TEST_ASSERT_GREATER_OR_EQUAL_INT(8, t.x);
        TEST_ASSERT_GREATER_OR_EQUAL_INT(8, t.y);
        TEST_ASSERT_LESS_OR_EQUAL_INT(448 - 8, t.x + t.side);
        TEST_ASSERT_LESS_OR_EQUAL_INT(368 - 8, t.y + t.side);
    }
}

/* Reaching both extremes is what shows targets cover the screen, not
 * merely stay on it. */
static void
test_targets_reach_every_edge_of_the_allowed_area(void) {
    uint32_t rng = 7;
    int min_x = 1000, max_x = -1, min_y = 1000, max_y = -1;
    for (int i = 0; i < 20000; i++) {
        const touch_probe_target_t t = touch_probe_next(&rng, 368, 448, 12, 8);
        min_x = t.x < min_x ? t.x : min_x;
        max_x = t.x > max_x ? t.x : max_x;
        min_y = t.y < min_y ? t.y : min_y;
        max_y = t.y > max_y ? t.y : max_y;
    }
    TEST_ASSERT_EQUAL_INT(8, min_x);
    TEST_ASSERT_EQUAL_INT(368 - 8 - 12, max_x);
    TEST_ASSERT_EQUAL_INT(8, min_y);
    TEST_ASSERT_EQUAL_INT(448 - 8 - 12, max_y);
}

static void
test_a_tap_on_the_centre_has_no_offset(void) {
    const touch_probe_target_t t = {.x = 100, .y = 200, .side = 12};
    int dx, dy;
    touch_probe_offset(t, 106, 206, &dx, &dy);
    TEST_ASSERT_EQUAL_INT(0, dx);
    TEST_ASSERT_EQUAL_INT(0, dy);
}

static void
test_a_tap_below_and_left_reads_positive_dy_negative_dx(void) {
    const touch_probe_target_t t = {.x = 100, .y = 200, .side = 12};
    int dx, dy;
    touch_probe_offset(t, 101, 226, &dx, &dy);
    TEST_ASSERT_EQUAL_INT(-5, dx);
    TEST_ASSERT_EQUAL_INT(20, dy);
}

static void
test_a_hit_is_inside_the_square_and_nowhere_else(void) {
    const touch_probe_target_t t = {.x = 100, .y = 200, .side = 12};
    touch_probe_stats_t s = {0};
    TEST_ASSERT_TRUE(touch_probe_record(&s, t, 100, 200));
    TEST_ASSERT_TRUE(touch_probe_record(&s, t, 111, 211));
    TEST_ASSERT_FALSE(touch_probe_record(&s, t, 112, 211));
    TEST_ASSERT_FALSE(touch_probe_record(&s, t, 111, 212));
    TEST_ASSERT_FALSE(touch_probe_record(&s, t, 99, 205));
    TEST_ASSERT_EQUAL_INT(5, s.taps);
    TEST_ASSERT_EQUAL_INT(2, s.hits);
}

/* dy of 10, 20, 30 around a centre at (106, 206): mean 20, sample spread 10. */
static void
test_mean_and_spread_follow_the_taps(void) {
    const touch_probe_target_t t = {.x = 100, .y = 200, .side = 12};
    touch_probe_stats_t s = {0};
    touch_probe_record(&s, t, 106, 216);
    touch_probe_record(&s, t, 106, 226);
    touch_probe_record(&s, t, 106, 236);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, touch_probe_mean_dx(&s));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 20.0f, touch_probe_mean_dy(&s));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, touch_probe_spread_dx(&s));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 10.0f, touch_probe_spread_dy(&s));
    TEST_ASSERT_EQUAL_INT(30 * 30, s.worst_dist2);
}

static void
test_no_taps_reads_as_zero_not_nan(void) {
    const touch_probe_stats_t s = {0};
    TEST_ASSERT_EQUAL_FLOAT(0.0f, touch_probe_mean_dy(&s));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, touch_probe_spread_dy(&s));
}

void
run_touch_probe_suite(void) {
    RUN_TEST(test_every_target_fits_inside_the_margin);
    RUN_TEST(test_targets_reach_every_edge_of_the_allowed_area);
    RUN_TEST(test_a_tap_on_the_centre_has_no_offset);
    RUN_TEST(test_a_tap_below_and_left_reads_positive_dy_negative_dx);
    RUN_TEST(test_a_hit_is_inside_the_square_and_nowhere_else);
    RUN_TEST(test_mean_and_spread_follow_the_taps);
    RUN_TEST(test_no_taps_reads_as_zero_not_nan);
}

SUITE_REGISTER(run_touch_probe_suite);
