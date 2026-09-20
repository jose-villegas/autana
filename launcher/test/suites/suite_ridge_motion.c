/*
 * Portable suite: ridge_motion.h - the ridge breathing, the wave along it,
 * and the momentum a slope gives that wave.
 */

#include <stdbool.h>
#include <stdlib.h>

#include "suites.h"
#include "unity.h"

#include "ui/ridge_motion.h"

#define COLUMNS 300

/* This suite's own fixture: not what ships (`TUNE(ridge, ...)` in
 * ui_ridge.c), just the numbers these assertions were written against. */
static const ridge_motion_params_t params = {
    .breath_ms = 9000,
    .breath_depth = 200,
    .wave_height_q4 = 40,
    .wave_length = 170,
    .wave_passes_in_ms = 2600,
    .push = 96,
    .coast_ms = 900,
};

/* Where the wave crosses zero going down, in the first wavelength. Its
 * crest is a plateau a dozen columns wide once rounded to whole Q4, and the
 * first of those jumps about; a crossing does not. */
static int
crest_column(const ridge_motion_t* motion) {
    for (int x = 0; x < params.wave_length; x++) {
        if (ridge_motion_wave(motion, &params, x) > 0 && ridge_motion_wave(motion, &params, x + 1) <= 0) {
            return x;
        }
    }
    return -1;
}

/* How many columns the crest moved toward the last column, the short way
 * round one wavelength. */
static int
crest_travel(int from, int to) {
    int moved = (to - from) % params.wave_length;
    if (moved > params.wave_length / 2) {
        moved -= params.wave_length;
    }
    if (moved < -params.wave_length / 2) {
        moved += params.wave_length;
    }
    return moved;
}

static void
test_a_breath_starts_rigid_swells_and_comes_back_rigid(void) {
    ridge_motion_t motion = {0};
    TEST_ASSERT_EQUAL_INT(0, ridge_motion_breath(&motion, &params));

    int deepest = 0;
    int last = 0;
    bool rose_then_fell = true;
    for (uint32_t ms = 0; ms < (uint32_t)params.breath_ms; ms += 50) {
        motion.breath_ms = ms;
        const int now = ridge_motion_breath(&motion, &params);
        TEST_ASSERT_TRUE(now >= 0 && now <= params.breath_depth);
        rose_then_fell = rose_then_fell && (ms <= (uint32_t)params.breath_ms / 2 ? now >= last : now <= last);
        deepest = now > deepest ? now : deepest;
        last = now;
    }
    TEST_ASSERT_TRUE(rose_then_fell);
    TEST_ASSERT_INT_WITHIN(1, params.breath_depth, deepest);

    ridge_motion_advance(&motion, &params, (uint32_t)params.breath_ms - motion.breath_ms, 0);
    TEST_ASSERT_EQUAL_INT(0, ridge_motion_breath(&motion, &params));
}

static void
test_the_height_is_the_rigid_ridge_plus_the_wave_at_the_top_of_a_breath(void) {
    ridge_motion_t motion = {0};
    for (int x = 0; x < COLUMNS; x += 13) {
        TEST_ASSERT_EQUAL_INT(2000 + ridge_motion_wave(&motion, &params, x),
                              ridge_motion_height(&motion, &params, 2000, 2600, x));
    }
    motion.breath_ms = (uint32_t)params.breath_ms / 2;
    const int halfway_out =
        ridge_motion_height(&motion, &params, 2000, 2600, 0) - ridge_motion_wave(&motion, &params, 0);
    TEST_ASSERT_INT_WITHIN(4, 2000 + 600 * params.breath_depth / 256, halfway_out);
}

static void
test_the_wave_stays_within_its_height_and_is_one_length_long(void) {
    ridge_motion_t motion = {0};
    ridge_motion_advance(&motion, &params, 777, 0);
    for (int x = 0; x < COLUMNS; x++) {
        const int h = ridge_motion_wave(&motion, &params, x);
        TEST_ASSERT_TRUE(h >= -params.wave_height_q4 && h <= params.wave_height_q4);
    }
    TEST_ASSERT_INT_WITHIN(2, ridge_motion_wave(&motion, &params, 20),
                           ridge_motion_wave(&motion, &params, 20 + params.wave_length));
}

static void
test_left_alone_the_wave_runs_toward_the_last_column_at_its_own_pace(void) {
    ridge_motion_t motion = {0};
    const int start = crest_column(&motion);
    ridge_motion_advance(&motion, &params, (uint32_t)params.wave_passes_in_ms / 4, 0);
    TEST_ASSERT_INT_WITHIN(3, params.wave_length / 4, crest_travel(start, crest_column(&motion)));
}

static void
test_a_slope_pushes_the_wave_and_it_coasts_after(void) {
    ridge_motion_t pushed = {0};
    ridge_motion_t left_alone = {0};
    const int start = crest_column(&pushed);
    for (int frame = 0; frame < 12; frame++) {
        ridge_motion_advance(&pushed, &params, 16, 8192);
        ridge_motion_advance(&left_alone, &params, 16, 0);
    }
    TEST_ASSERT_GREATER_THAN_INT(0, pushed.momentum_q8);
    TEST_ASSERT_GREATER_THAN_INT(crest_travel(start, crest_column(&left_alone)),
                                 crest_travel(start, crest_column(&pushed)));

    const int32_t when_let_go = pushed.momentum_q8;
    ridge_motion_advance(&pushed, &params, 16, 0);
    TEST_ASSERT_TRUE(pushed.momentum_q8 > 0 && pushed.momentum_q8 < when_let_go);
    for (int frame = 0; frame < 1000; frame++) {
        ridge_motion_advance(&pushed, &params, 16, 0);
    }
    TEST_ASSERT_EQUAL_INT32(0, pushed.momentum_q8);
}

static void
test_a_slope_the_other_way_can_turn_the_wave_back(void) {
    ridge_motion_t motion = {0};
    for (int frame = 0; frame < 60; frame++) {
        ridge_motion_advance(&motion, &params, 16, -16384);
    }
    const int before = crest_column(&motion);
    ridge_motion_advance(&motion, &params, 100, -16384);
    TEST_ASSERT_LESS_THAN_INT(0, crest_travel(before, crest_column(&motion)));
}

static void
test_momentum_is_bounded_however_long_the_slope_lasts(void) {
    ridge_motion_t motion = {0};
    for (int frame = 0; frame < 5000; frame++) {
        ridge_motion_advance(&motion, &params, 16, 16384);
        TEST_ASSERT_TRUE(motion.momentum_q8 <= RIDGE_SPEED_MAX);
    }
}

static void
test_smoothing_leaves_a_flat_line_alone_and_rounds_a_step(void) {
    int16_t* in = malloc(sizeof(int16_t) * COLUMNS * 3);
    TEST_ASSERT_NOT_NULL(in);
    int16_t* out = in + COLUMNS;
    int16_t* scratch = in + 2 * COLUMNS;

    for (int x = 0; x < COLUMNS; x++) {
        in[x] = 1234;
    }
    ridge_motion_smooth(in, out, scratch, COLUMNS, RIDGE_SMOOTH_RADIUS);
    for (int x = 0; x < COLUMNS; x++) {
        TEST_ASSERT_EQUAL_INT16(1234, out[x]);
    }

    for (int x = 0; x < COLUMNS; x++) {
        in[x] = x < COLUMNS / 2 ? 1000 : 3000;
    }
    ridge_motion_smooth(in, out, scratch, COLUMNS, RIDGE_SMOOTH_RADIUS);
    TEST_ASSERT_EQUAL_INT16(1000, out[10]);
    TEST_ASSERT_EQUAL_INT16(3000, out[COLUMNS - 10]);
    TEST_ASSERT_INT_WITHIN(60, 2000, out[COLUMNS / 2]);
    int steepest = 0;
    for (int x = 1; x < COLUMNS; x++) {
        TEST_ASSERT_TRUE(out[x] >= out[x - 1]);
        steepest = out[x] - out[x - 1] > steepest ? out[x] - out[x - 1] : steepest;
    }
    TEST_ASSERT_LESS_THAN_INT(2000 / RIDGE_SMOOTH_RADIUS, steepest);
    free(in);
}

static void
test_extending_keeps_the_middle_and_runs_each_end_out_level(void) {
    enum { COUNT = 100, EXTRA = 40 };

    int16_t in[COUNT];
    int16_t out[COUNT + 2 * EXTRA];
    for (int x = 0; x < COUNT; x++) {
        in[x] = (int16_t)(3000 - 20 * x); /* rises steadily toward the last column */
    }
    ridge_motion_extend(in, COUNT, out, EXTRA);

    for (int x = 0; x < COUNT; x++) {
        TEST_ASSERT_EQUAL_INT16(in[x], out[EXTRA + x]);
    }
    /* Each end carries on the way it was going: down to the left, up to the
     * right, a whole step at first and none by the end. */
    TEST_ASSERT_INT_WITHIN(3, 20, out[EXTRA - 1] - out[EXTRA]);
    TEST_ASSERT_INT_WITHIN(3, -20, out[EXTRA + COUNT] - out[EXTRA + COUNT - 1]);
    TEST_ASSERT_INT_WITHIN(1, 0, out[0] - out[1]);
    TEST_ASSERT_INT_WITHIN(1, 0, out[COUNT + 2 * EXTRA - 1] - out[COUNT + 2 * EXTRA - 2]);
    for (int k = 1; k < EXTRA; k++) {
        TEST_ASSERT_TRUE(out[EXTRA - k - 1] >= out[EXTRA - k]);
        TEST_ASSERT_TRUE(out[EXTRA + COUNT - 1 + k + 1] <= out[EXTRA + COUNT - 1 + k]);
    }
    /* A slope eased to nothing over EXTRA columns covers half what it would
     * have at full tilt. */
    TEST_ASSERT_INT_WITHIN(12, 20 * EXTRA / 2, out[0] - in[0]);
}

static void
test_extending_by_nothing_is_a_copy(void) {
    int16_t in[5] = {5, 4, 3, 2, 1};
    int16_t out[5] = {0};
    ridge_motion_extend(in, 5, out, 0);
    TEST_ASSERT_EQUAL_INT16_ARRAY(in, out, 5);
}

static void
test_easing_in_starts_and_ends_gently_and_is_half_way_at_half_time(void) {
    TEST_ASSERT_EQUAL_INT(0, ridge_motion_ease_in(0, 4000));
    TEST_ASSERT_EQUAL_INT(128, ridge_motion_ease_in(2000, 4000));
    TEST_ASSERT_EQUAL_INT(256, ridge_motion_ease_in(4000, 4000));
    TEST_ASSERT_EQUAL_INT(256, ridge_motion_ease_in(9000, 4000));
    TEST_ASSERT_EQUAL_INT(256, ridge_motion_ease_in(0, 0));

    const int first_tenth = ridge_motion_ease_in(400, 4000);
    const int middle_tenth = ridge_motion_ease_in(2200, 4000) - ridge_motion_ease_in(1800, 4000);
    const int last_tenth = 256 - ridge_motion_ease_in(3600, 4000);
    TEST_ASSERT_TRUE(first_tenth * 3 < middle_tenth);
    TEST_ASSERT_TRUE(last_tenth * 3 < middle_tenth);

    for (uint32_t ms = 1; ms <= 4000; ms++) {
        TEST_ASSERT_TRUE(ridge_motion_ease_in(ms, 4000) >= ridge_motion_ease_in(ms - 1, 4000));
    }
}

void
suite_ridge_motion(void) {
    RUN_TEST(test_a_breath_starts_rigid_swells_and_comes_back_rigid);
    RUN_TEST(test_the_height_is_the_rigid_ridge_plus_the_wave_at_the_top_of_a_breath);
    RUN_TEST(test_the_wave_stays_within_its_height_and_is_one_length_long);
    RUN_TEST(test_left_alone_the_wave_runs_toward_the_last_column_at_its_own_pace);
    RUN_TEST(test_a_slope_pushes_the_wave_and_it_coasts_after);
    RUN_TEST(test_a_slope_the_other_way_can_turn_the_wave_back);
    RUN_TEST(test_momentum_is_bounded_however_long_the_slope_lasts);
    RUN_TEST(test_smoothing_leaves_a_flat_line_alone_and_rounds_a_step);
    RUN_TEST(test_extending_keeps_the_middle_and_runs_each_end_out_level);
    RUN_TEST(test_extending_by_nothing_is_a_copy);
    RUN_TEST(test_easing_in_starts_and_ends_gently_and_is_half_way_at_half_time);
}

SUITE_REGISTER(suite_ridge_motion);
