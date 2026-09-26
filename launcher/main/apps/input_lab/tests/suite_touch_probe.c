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

static void
test_a_grid_spans_the_allowed_area_corner_to_corner(void) {
    const touch_probe_target_t first = touch_probe_grid(0, 5, 5, 448, 368, 12, 8);
    const touch_probe_target_t last = touch_probe_grid(24, 5, 5, 448, 368, 12, 8);
    TEST_ASSERT_EQUAL_INT(8, first.x);
    TEST_ASSERT_EQUAL_INT(8, first.y);
    TEST_ASSERT_EQUAL_INT(448 - 8 - 12, last.x);
    TEST_ASSERT_EQUAL_INT(368 - 8 - 12, last.y);
}

/* Index runs along a row first: 7 in a 5x5 grid is row 1, column 2, the
 * middle of the allowed width. */
static void
test_a_grid_index_runs_along_a_row_first(void) {
    const touch_probe_target_t t = touch_probe_grid(7, 5, 5, 448, 368, 12, 8);
    TEST_ASSERT_EQUAL_INT(8 + (448 - 16 - 12) / 2, t.x);
    TEST_ASSERT_EQUAL_INT(8 + (368 - 16 - 12) / 4, t.y);
}

static void
test_a_shuffle_visits_every_index_once(void) {
    uint32_t rng = 3;
    int order[25];
    touch_probe_shuffle(&rng, order, 25);
    int seen[25] = {0};
    int in_place = 0;
    for (int i = 0; i < 25; i++) {
        TEST_ASSERT_TRUE(order[i] >= 0 && order[i] < 25);
        seen[order[i]]++;
        in_place += order[i] == i;
    }
    for (int i = 0; i < 25; i++) {
        TEST_ASSERT_EQUAL_INT(1, seen[i]);
    }
    TEST_ASSERT_LESS_THAN_INT_MESSAGE(25, in_place, "a shuffle must move something");
}

/* The window 30-80 ms holds x 10, 30, 20: the median is 20, whatever
 * came before or after. */
static void
test_the_settled_point_is_the_median_inside_the_window(void) {
    const touch_probe_sample_t s[] = {
        {.x = 0, .y = 0, .t_ms = 0},   {.x = 10, .y = 5, .t_ms = 30},  {.x = 30, .y = 1, .t_ms = 50},
        {.x = 20, .y = 3, .t_ms = 70}, {.x = 99, .y = 99, .t_ms = 90},
    };
    int x, y;
    touch_probe_settled(s, 5, 30, 80, &x, &y);
    TEST_ASSERT_EQUAL_INT(20, x);
    TEST_ASSERT_EQUAL_INT(3, y);
}

/* A tap lifted before the window opens has no settled sample of its own;
 * the last one it did have stands in. */
static void
test_a_tap_too_short_for_the_window_settles_on_its_last_sample(void) {
    const touch_probe_sample_t s[] = {{.x = 5, .y = 6, .t_ms = 0}, {.x = 7, .y = 8, .t_ms = 16}};
    int x, y;
    touch_probe_settled(s, 2, 30, 80, &x, &y);
    TEST_ASSERT_EQUAL_INT(7, x);
    TEST_ASSERT_EQUAL_INT(8, y);
}

/* The window's own bounds count: samples at exactly 30 and 80 ms are in it. */
static void
test_the_settle_window_includes_both_bounds(void) {
    const touch_probe_sample_t s[] = {
        {.x = 1, .y = 1, .t_ms = 29}, {.x = 40, .y = 40, .t_ms = 30}, {.x = 50, .y = 50, .t_ms = 80},
        {.x = 9, .y = 9, .t_ms = 81}, {.x = 45, .y = 45, .t_ms = 55},
    };
    int x, y;
    touch_probe_settled(s, 5, 30, 80, &x, &y);
    TEST_ASSERT_EQUAL_INT(45, x);
    TEST_ASSERT_EQUAL_INT(45, y);
}

static const input_t IDLE = {0};

/* Static: a tap's sample buffer is too large for the device's test stack. */
static touch_probe_tap_t tap;

static touch_probe_tap_t*
fresh_tap(void) {
    tap = (touch_probe_tap_t){0};
    return &tap;
}

static input_t
press_at(int x, int y) {
    return (input_t){.pressed = true, .down = true, .press_x = x, .press_y = y, .x = x, .y = y};
}

static input_t
held_at(int press_x, int press_y, int x, int y) {
    return (input_t){.down = true, .press_x = press_x, .press_y = press_y, .x = x, .y = y};
}

static input_t
released_at(int press_x, int press_y, int x, int y) {
    return (input_t){.released = true, .press_x = press_x, .press_y = press_y, .x = x, .y = y};
}

/* A tap held over three frames, drifting: every position in order, timed
 * from the press, and one completion on the release. */
static void
test_a_held_tap_is_followed_frame_by_frame(void) {
    touch_probe_tap_t* t = fresh_tap();
    TEST_ASSERT_FALSE(touch_probe_track(t, 16, &IDLE));
    TEST_ASSERT_FALSE(touch_probe_track(t, 16, &IDLE));
    const input_t press = press_at(100, 200);
    TEST_ASSERT_FALSE(touch_probe_track(t, 16, &press));
    const input_t held1 = held_at(100, 200, 102, 201);
    TEST_ASSERT_FALSE(touch_probe_track(t, 10, &held1));
    const input_t held2 = held_at(100, 200, 104, 203);
    TEST_ASSERT_FALSE(touch_probe_track(t, 10, &held2));
    const input_t release = released_at(100, 200, 104, 203);
    TEST_ASSERT_TRUE(touch_probe_track(t, 10, &release));

    TEST_ASSERT_EQUAL_INT(3, t->count);
    TEST_ASSERT_EQUAL_INT(100, t->samples[0].x);
    TEST_ASSERT_EQUAL_INT(0, t->samples[0].t_ms);
    TEST_ASSERT_EQUAL_INT(102, t->samples[1].x);
    TEST_ASSERT_EQUAL_INT(10, t->samples[1].t_ms);
    TEST_ASSERT_EQUAL_INT(203, t->samples[2].y);
    TEST_ASSERT_EQUAL_INT(20, t->samples[2].t_ms);
    TEST_ASSERT_EQUAL_INT(32, t->idle_before_press);
    TEST_ASSERT_FALSE_MESSAGE(touch_probe_track(t, 16, &IDLE), "a tap completes once");
}

static void
test_a_press_and_release_in_one_frame_is_a_tap(void) {
    touch_probe_tap_t* t = fresh_tap();
    input_t both = press_at(50, 60);
    both.released = true;
    both.down = false;
    TEST_ASSERT_TRUE(touch_probe_track(t, 16, &both));
    TEST_ASSERT_EQUAL_INT(1, t->count);
    TEST_ASSERT_EQUAL_INT(50, t->samples[0].x);
}

/* A press that lands where the finger then already moved on keeps both. */
static void
test_a_press_already_moved_keeps_where_it_began(void) {
    touch_probe_tap_t* t = fresh_tap();
    input_t press = press_at(50, 60);
    press.x = 55;
    touch_probe_track(t, 16, &press);
    TEST_ASSERT_EQUAL_INT(2, t->count);
    TEST_ASSERT_EQUAL_INT(50, t->samples[0].x);
    TEST_ASSERT_EQUAL_INT(55, t->samples[1].x);
}

/* A lift with no press before it - a finger already down when counting
 * began - is not a tap. */
static void
test_a_release_without_its_press_is_no_tap(void) {
    touch_probe_tap_t* t = fresh_tap();
    const input_t release = released_at(10, 10, 10, 10);
    TEST_ASSERT_FALSE(touch_probe_track(t, 16, &release));
}

static void
test_a_long_hold_keeps_to_the_sample_buffer(void) {
    touch_probe_tap_t* t = fresh_tap();
    const input_t press = press_at(1, 1);
    touch_probe_track(t, 16, &press);
    const input_t held = held_at(1, 1, 2, 2);
    for (int i = 0; i < 3 * TOUCH_PROBE_SAMPLES_MAX; i++) {
        touch_probe_track(t, 5, &held);
    }
    TEST_ASSERT_EQUAL_INT(TOUCH_PROBE_SAMPLES_MAX, t->count);
    TEST_ASSERT_EQUAL_INT(3 * TOUCH_PROBE_SAMPLES_MAX * 5, t->held_ms);
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
    RUN_TEST(test_a_grid_spans_the_allowed_area_corner_to_corner);
    RUN_TEST(test_a_grid_index_runs_along_a_row_first);
    RUN_TEST(test_a_shuffle_visits_every_index_once);
    RUN_TEST(test_the_settled_point_is_the_median_inside_the_window);
    RUN_TEST(test_a_tap_too_short_for_the_window_settles_on_its_last_sample);
    RUN_TEST(test_the_settle_window_includes_both_bounds);
    RUN_TEST(test_a_held_tap_is_followed_frame_by_frame);
    RUN_TEST(test_a_press_and_release_in_one_frame_is_a_tap);
    RUN_TEST(test_a_press_already_moved_keeps_where_it_began);
    RUN_TEST(test_a_release_without_its_press_is_no_tap);
    RUN_TEST(test_a_long_hold_keeps_to_the_sample_buffer);
}

SUITE_REGISTER(run_touch_probe_suite);
