/* The painter's time-driven phases: each advances by elapsed time, carries the remainder, and reports a move. */
#include <stdint.h>
#include <stdlib.h>

#include "suites.h"
#include "unity.h"

#include "apps/sand/sand_paint_clock.h"

typedef struct {
    const char* name;
    uint32_t dt_ms[4];
    int moved_on[4]; /* 1 where that call must report a move */
    int offset_after;
} shine_case_t;

static void
test_the_shine_steps_two_px_per_whole_period_and_carries_the_rest(void) {
    static const shine_case_t cases[] = {
        {"under one period", {39, 0, 0, 0}, {0, 0, 0, 0}, 0},
        {"exactly one", {40, 0, 0, 0}, {1, 0, 0, 0}, SAND_PAINT_SHINE_STEP_PX},
        {"a remainder carries", {30, 30, 0, 0}, {0, 1, 0, 0}, SAND_PAINT_SHINE_STEP_PX},
        {"several in one call", {125, 0, 0, 0}, {1, 0, 0, 0}, 3 * SAND_PAINT_SHINE_STEP_PX},
        {"wraps at the period", {40 * 32, 40, 0, 0}, {1, 1, 0, 0}, SAND_PAINT_SHINE_STEP_PX},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        sand_paint_clock_t* c = malloc(sizeof *c);
        TEST_ASSERT_NOT_NULL(c);
        *c = (sand_paint_clock_t)SAND_PAINT_CLOCK_INIT;
        sand_paint_frame_t pf = SAND_PAINT_FRAME_INIT;
        for (int k = 0; k < 4; k++) {
            TEST_ASSERT_EQUAL_INT_MESSAGE(cases[i].moved_on[k], sand_paint_clock_shine(c, &pf, cases[i].dt_ms[k]),
                                          cases[i].name);
        }
        TEST_ASSERT_EQUAL_INT_MESSAGE(cases[i].offset_after, pf.shine_offset, cases[i].name);
        free(c);
    }
}

typedef struct {
    const char* name;
    uint32_t dt_ms;
    int cullet_moved;
    unsigned cullet_phase;
    int wood_leaf_moved;
    int local_depth_moved;
} wake_case_t;

static void
test_each_wake_clock_reports_a_move_only_on_a_whole_period(void) {
    static const wake_case_t cases[] = {
        {"nothing elapsed", 0, 0, 0, 0, 0},      {"one leaf period", 40, 0, 0, 1, 0},
        {"one depth period", 120, 0, 0, 1, 1},   {"one cullet period", 250, 1, 1, 1, 1},
        {"two cullet periods", 500, 1, 2, 1, 1},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        sand_paint_clock_t* c = malloc(sizeof *c);
        TEST_ASSERT_NOT_NULL(c);
        *c = (sand_paint_clock_t)SAND_PAINT_CLOCK_INIT;
        sand_paint_frame_t pf = SAND_PAINT_FRAME_INIT;
        TEST_ASSERT_EQUAL_INT_MESSAGE(cases[i].cullet_moved, sand_paint_clock_cullet(c, cases[i].dt_ms), cases[i].name);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(cases[i].cullet_phase, c->cullet_phase, cases[i].name);
        TEST_ASSERT_EQUAL_INT_MESSAGE(cases[i].wood_leaf_moved, sand_paint_clock_wood_leaf(c, &pf, cases[i].dt_ms),
                                      cases[i].name);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(cases[i].dt_ms, pf.wood_leaf_time_ms, cases[i].name);
        TEST_ASSERT_EQUAL_INT_MESSAGE(cases[i].local_depth_moved, sand_paint_clock_local_depth(c, cases[i].dt_ms),
                                      cases[i].name);
        free(c);
    }
}

static void
test_the_wind_flips_at_its_due_time_and_the_next_due_time_is_jittered(void) {
    sand_paint_clock_t* c = malloc(sizeof *c);
    TEST_ASSERT_NOT_NULL(c);
    *c = (sand_paint_clock_t)SAND_PAINT_CLOCK_INIT;
    sand_paint_frame_t pf = SAND_PAINT_FRAME_INIT;

    sand_paint_clock_wind(c, &pf, SAND_PAINT_WIND_FLIP_BASE_MS - 1);
    TEST_ASSERT_EQUAL_INT(1, pf.wood_leaf_wind_sign);
    sand_paint_clock_wind(c, &pf, 1);
    TEST_ASSERT_EQUAL_INT(-1, pf.wood_leaf_wind_sign);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(SAND_PAINT_WIND_FLIP_BASE_MS, c->wind_flip_due_ms);
    TEST_ASSERT_LESS_THAN_UINT32(SAND_PAINT_WIND_FLIP_BASE_MS + SAND_PAINT_WIND_FLIP_JITTER_MS, c->wind_flip_due_ms);

    sand_paint_clock_wind(c, &pf, c->wind_flip_due_ms);
    TEST_ASSERT_EQUAL_INT(1, pf.wood_leaf_wind_sign);
    free(c);
}

typedef struct {
    int gx, gy;
    int bearing_q16;
} bearing_case_t;

static void
test_the_gravity_bearing_is_a_quarter_turn_per_axis_and_zero_without_gravity(void) {
    static const bearing_case_t cases[] = {
        {0, 0, 0}, {0, 256, 65536}, {256, 0, 0}, {0, -256, -65536}, {-256, 0, 131072}, {256, 256, 32768},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        TEST_ASSERT_EQUAL_INT(cases[i].bearing_q16, sand_paint_gravity_bearing_q16(cases[i].gx, cases[i].gy));
    }
}

static void
test_the_glass_phase_reports_a_move_only_when_the_bearing_changes(void) {
    sand_paint_clock_t* c = malloc(sizeof *c);
    TEST_ASSERT_NOT_NULL(c);
    *c = (sand_paint_clock_t)SAND_PAINT_CLOCK_INIT;
    TEST_ASSERT_TRUE(sand_paint_clock_glass(c, 0, 256));
    TEST_ASSERT_FALSE(sand_paint_clock_glass(c, 0, 512));
    TEST_ASSERT_TRUE(sand_paint_clock_glass(c, 256, 256));
    free(c);
}

void
run_sand_paint_clock_suite(void) {
    RUN_TEST(test_the_shine_steps_two_px_per_whole_period_and_carries_the_rest);
    RUN_TEST(test_each_wake_clock_reports_a_move_only_on_a_whole_period);
    RUN_TEST(test_the_wind_flips_at_its_due_time_and_the_next_due_time_is_jittered);
    RUN_TEST(test_the_gravity_bearing_is_a_quarter_turn_per_axis_and_zero_without_gravity);
    RUN_TEST(test_the_glass_phase_reports_a_move_only_when_the_bearing_changes);
}

SUITE_REGISTER(run_sand_paint_clock_suite);
