/* The painter's time-driven phases: each advances by elapsed time, carries the remainder, and reports a move. */
#include <stdint.h>
#include <stdlib.h>

#include "suites.h"
#include "unity.h"

#include "apps/sand/sand_paint_clock.h"
#include "util/scalar/mathx.h"

#define SHINE_MS               SAND_PAINT_SHINE_STEP_MS
#define SHINE_STEPS_PER_PERIOD (SAND_PAINT_SHINE_PERIOD / SAND_PAINT_SHINE_STEP_PX)

typedef struct {
    const char* name;
    uint32_t dt_ms[4];
    int moved_on[4]; /* 1 where that call must report a move */
    int offset_after;
} shine_case_t;

static void
test_the_shine_steps_two_px_per_whole_period_and_carries_the_rest(void) {
    static const shine_case_t cases[] = {
        {"under one period", {SHINE_MS - 1, 0, 0, 0}, {0, 0, 0, 0}, 0},
        {"exactly one", {SHINE_MS, 0, 0, 0}, {1, 0, 0, 0}, SAND_PAINT_SHINE_STEP_PX},
        {"a remainder carries", {SHINE_MS * 3 / 4, SHINE_MS * 3 / 4, 0, 0}, {0, 1, 0, 0}, SAND_PAINT_SHINE_STEP_PX},
        {"several in one call", {(3 * SHINE_MS) + 1, 0, 0, 0}, {1, 0, 0, 0}, 3 * SAND_PAINT_SHINE_STEP_PX},
        {"wraps at the period",
         {SHINE_MS * SHINE_STEPS_PER_PERIOD, SHINE_MS, 0, 0},
         {1, 1, 0, 0},
         SAND_PAINT_SHINE_STEP_PX},
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
        {"nothing elapsed", 0, 0, 0, 0, 0},
        {"one leaf period", SAND_PAINT_WOOD_LEAF_WAKE_MS, 0, 0, 1, 0},
        {"one depth period", SAND_PAINT_LOCAL_DEPTH_WAKE_MS, 0, 0, 1, 1},
        {"one cullet period", SAND_PAINT_CULLET_PHASE_MS, 1, 1, 1, 1},
        {"two cullet periods", 2 * SAND_PAINT_CULLET_PHASE_MS, 1, 2, 1, 1},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        sand_paint_clock_t* c = malloc(sizeof *c);
        TEST_ASSERT_NOT_NULL(c);
        *c = (sand_paint_clock_t)SAND_PAINT_CLOCK_INIT;
        sand_paint_frame_t pf = SAND_PAINT_FRAME_INIT;
        TEST_ASSERT_EQUAL_INT_MESSAGE(cases[i].cullet_moved, sand_paint_clock_cullet(c, &pf, cases[i].dt_ms),
                                      cases[i].name);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(cases[i].cullet_phase, pf.material.cullet_phase, cases[i].name);
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
        {0, 0, 0},
        {0, 256, MATHX_ONE},
        {256, 0, 0},
        {0, -256, -MATHX_ONE},
        {-256, 0, 2 * MATHX_ONE},
        {256, 256, MATHX_ONE / 2},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        TEST_ASSERT_EQUAL_INT(cases[i].bearing_q16, sand_paint_gravity_bearing_q16(cases[i].gx, cases[i].gy));
    }
}

static void
test_the_glass_phase_reports_a_move_only_when_the_bearing_changes(void) {
    sand_paint_frame_t pf = SAND_PAINT_FRAME_INIT;
    TEST_ASSERT_TRUE(sand_paint_clock_glass(&pf, 0, 256));
    TEST_ASSERT_FALSE(sand_paint_clock_glass(&pf, 0, 512));
    TEST_ASSERT_TRUE(sand_paint_clock_glass(&pf, 256, 256));
    TEST_ASSERT_EQUAL_INT(sand_paint_gravity_bearing_q16(256, 256) >> SAND_PAINT_GLASS_PHASE_SHIFT,
                          pf.material.glass_phase);
}

static void
test_the_foam_phase_counts_whole_periods_since_the_start(void) {
    sand_paint_clock_t* c = malloc(sizeof *c);
    TEST_ASSERT_NOT_NULL(c);
    *c = (sand_paint_clock_t)SAND_PAINT_CLOCK_INIT;
    sand_paint_frame_t pf = SAND_PAINT_FRAME_INIT;
    sand_paint_clock_foam(c, &pf, SAND_PAINT_FOAM_PHASE_MS - 1);
    TEST_ASSERT_EQUAL_UINT(0, pf.material.foam_phase);
    sand_paint_clock_foam(c, &pf, (2 * SAND_PAINT_FOAM_PHASE_MS) + 1);
    TEST_ASSERT_EQUAL_UINT(3, pf.material.foam_phase);
    free(c);
}

/* Time past a firing call counts toward the next period, for every clock built on the period count. */
static void
test_the_time_past_a_firing_call_counts_toward_the_next_period(void) {
    static const uint32_t periods_ms[] = {SAND_PAINT_SHINE_STEP_MS, SAND_PAINT_CULLET_PHASE_MS,
                                          SAND_PAINT_WOOD_LEAF_WAKE_MS, SAND_PAINT_LOCAL_DEPTH_WAKE_MS};
    for (size_t i = 0; i < sizeof periods_ms / sizeof periods_ms[0]; i++) {
        const uint32_t p = periods_ms[i];
        const uint32_t over = p / 4;
        uint32_t elapsed_ms = 0;
        TEST_ASSERT_EQUAL_UINT32(1, sand_paint_clock_periods(&elapsed_ms, p + over, p));
        TEST_ASSERT_EQUAL_UINT32(1, sand_paint_clock_periods(&elapsed_ms, p - over, p));
        TEST_ASSERT_EQUAL_UINT32(0, elapsed_ms);
    }
}

/* Phases and offsets build on what earlier calls left, never only the latest call. */
static void
test_the_shine_cullet_and_leaf_time_add_up_across_calls(void) {
    sand_paint_clock_t* c = malloc(sizeof *c);
    TEST_ASSERT_NOT_NULL(c);
    *c = (sand_paint_clock_t)SAND_PAINT_CLOCK_INIT;
    sand_paint_frame_t pf = SAND_PAINT_FRAME_INIT;

    TEST_ASSERT_TRUE(sand_paint_clock_shine(c, &pf, SHINE_MS));
    TEST_ASSERT_TRUE(sand_paint_clock_shine(c, &pf, SHINE_MS));
    TEST_ASSERT_EQUAL_INT(2 * SAND_PAINT_SHINE_STEP_PX, pf.shine_offset);

    TEST_ASSERT_TRUE(sand_paint_clock_cullet(c, &pf, SAND_PAINT_CULLET_PHASE_MS));
    TEST_ASSERT_TRUE(sand_paint_clock_cullet(c, &pf, SAND_PAINT_CULLET_PHASE_MS));
    TEST_ASSERT_EQUAL_UINT(2, pf.material.cullet_phase);

    (void)sand_paint_clock_wood_leaf(c, &pf, SAND_PAINT_WOOD_LEAF_WAKE_MS);
    (void)sand_paint_clock_wood_leaf(c, &pf, 1);
    TEST_ASSERT_EQUAL_UINT32(SAND_PAINT_WOOD_LEAF_WAKE_MS + 1, pf.wood_leaf_time_ms);
    free(c);
}

/* Foam counts whole periods of the total time, not of each call: three half periods are one and a half. */
static void
test_the_foam_phase_counts_whole_periods_of_the_total_time(void) {
    sand_paint_clock_t* c = malloc(sizeof *c);
    TEST_ASSERT_NOT_NULL(c);
    *c = (sand_paint_clock_t)SAND_PAINT_CLOCK_INIT;
    sand_paint_frame_t pf = SAND_PAINT_FRAME_INIT;
    for (int k = 0; k < 3; k++) {
        sand_paint_clock_foam(c, &pf, SAND_PAINT_FOAM_PHASE_MS / 2);
    }
    TEST_ASSERT_EQUAL_UINT(1, pf.material.foam_phase);
    free(c);
}

/* A late flip shortens the next wait by its lateness. */
static void
test_the_wind_carries_the_time_past_a_flip(void) {
    sand_paint_clock_t* c = malloc(sizeof *c);
    TEST_ASSERT_NOT_NULL(c);
    *c = (sand_paint_clock_t)SAND_PAINT_CLOCK_INIT;
    sand_paint_frame_t pf = SAND_PAINT_FRAME_INIT;
    const uint32_t late_ms = 100;

    sand_paint_clock_wind(c, &pf, SAND_PAINT_WIND_FLIP_BASE_MS + late_ms);
    TEST_ASSERT_EQUAL_INT(-1, pf.wood_leaf_wind_sign);
    sand_paint_clock_wind(c, &pf, c->wind_flip_due_ms - late_ms - 1);
    TEST_ASSERT_EQUAL_INT(-1, pf.wood_leaf_wind_sign);
    sand_paint_clock_wind(c, &pf, 1);
    TEST_ASSERT_EQUAL_INT(1, pf.wood_leaf_wind_sign);
    free(c);
}

/* Not metronomic: over several flips the wait varies, always within its range. */
#define WIND_FLIPS 8

static void
test_the_wind_wait_varies_from_flip_to_flip(void) {
    sand_paint_clock_t* c = malloc(sizeof *c);
    TEST_ASSERT_NOT_NULL(c);
    *c = (sand_paint_clock_t)SAND_PAINT_CLOCK_INIT;
    sand_paint_frame_t pf = SAND_PAINT_FRAME_INIT;
    uint32_t first_due_ms = 0;
    int distinct = 0;
    for (int k = 0; k < WIND_FLIPS; k++) {
        sand_paint_clock_wind(c, &pf, c->wind_flip_due_ms);
        TEST_ASSERT_GREATER_OR_EQUAL_UINT32(SAND_PAINT_WIND_FLIP_BASE_MS, c->wind_flip_due_ms);
        TEST_ASSERT_LESS_THAN_UINT32(SAND_PAINT_WIND_FLIP_BASE_MS + SAND_PAINT_WIND_FLIP_JITTER_MS,
                                     c->wind_flip_due_ms);
        if (k == 0) {
            first_due_ms = c->wind_flip_due_ms;
        } else if (c->wind_flip_due_ms != first_due_ms) {
            distinct++;
        }
    }
    TEST_ASSERT_GREATER_THAN_INT(0, distinct);
    free(c);
}

void
run_sand_paint_clock_suite(void) {
    RUN_TEST(test_the_shine_steps_two_px_per_whole_period_and_carries_the_rest);
    RUN_TEST(test_each_wake_clock_reports_a_move_only_on_a_whole_period);
    RUN_TEST(test_the_wind_flips_at_its_due_time_and_the_next_due_time_is_jittered);
    RUN_TEST(test_the_gravity_bearing_is_a_quarter_turn_per_axis_and_zero_without_gravity);
    RUN_TEST(test_the_glass_phase_reports_a_move_only_when_the_bearing_changes);
    RUN_TEST(test_the_foam_phase_counts_whole_periods_since_the_start);
    RUN_TEST(test_the_time_past_a_firing_call_counts_toward_the_next_period);
    RUN_TEST(test_the_shine_cullet_and_leaf_time_add_up_across_calls);
    RUN_TEST(test_the_foam_phase_counts_whole_periods_of_the_total_time);
    RUN_TEST(test_the_wind_carries_the_time_past_a_flip);
    RUN_TEST(test_the_wind_wait_varies_from_flip_to_flip);
}

SUITE_REGISTER(run_sand_paint_clock_suite);
