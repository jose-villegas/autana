/*
 * Portable suite: rt_pixel_budget.h - the shared growth/shrink step
 * scene_raytrace.c's progressive trace paces itself with. A single
 * over-budget frame nearly halves the budget, while recovery only grows it
 * back one step at a time - a landscape resolve ran about two seconds
 * slower than portrait because a tiny per-frame difference in one
 * orientation was amplified into a much longer resolve.
 */

#include "suites.h"
#include "unity.h"

#include "rt_pixel_budget.h"

#define STEP            8
#define MIN             1
#define MAX             1000
#define TARGET_FRAME_MS 30
#define UNDER_BUDGET_MS (TARGET_FRAME_MS - 1)
#define OVER_BUDGET_MS  (TARGET_FRAME_MS + 1)

static rt_pixel_budget_t
fixture(void) {
    return rt_pixel_budget_init(STEP, MIN, MAX, TARGET_FRAME_MS);
}

static void
test_grows_by_one_step_per_under_budget_frame(void) {
    rt_pixel_budget_t budget = fixture();

    rt_pixel_budget_adapt(&budget, UNDER_BUDGET_MS);
    TEST_ASSERT_EQUAL_INT(2 * STEP, budget.value);
    rt_pixel_budget_adapt(&budget, UNDER_BUDGET_MS);
    TEST_ASSERT_EQUAL_INT(3 * STEP, budget.value);
}

static void
test_growth_stops_at_max(void) {
    rt_pixel_budget_t budget = rt_pixel_budget_init(STEP, MIN, 2 * STEP, TARGET_FRAME_MS);

    rt_pixel_budget_adapt(&budget, UNDER_BUDGET_MS);
    rt_pixel_budget_adapt(&budget, UNDER_BUDGET_MS);
    rt_pixel_budget_adapt(&budget, UNDER_BUDGET_MS);
    TEST_ASSERT_EQUAL_INT(2 * STEP, budget.value);
}

static void
test_shrink_never_drops_below_min(void) {
    rt_pixel_budget_t budget = rt_pixel_budget_init(STEP, MIN, MAX, TARGET_FRAME_MS);

    rt_pixel_budget_adapt(&budget, OVER_BUDGET_MS);
    rt_pixel_budget_adapt(&budget, OVER_BUDGET_MS);
    rt_pixel_budget_adapt(&budget, OVER_BUDGET_MS);
    TEST_ASSERT_EQUAL_INT(MIN, budget.value);
}

/* The symptom: grow the budget in over many good frames, take exactly one
 * over-budget frame, then count how many good frames it takes to reach the
 * pre-drop value again. Bounded recovery means that count never depends on
 * how high the budget had grown - one bad frame always costs one good
 * frame, the same way it always costs one step going up. */
static void
test_one_over_budget_frame_costs_one_frame_to_recover(void) {
    rt_pixel_budget_t budget = rt_pixel_budget_init(STEP, MIN, 1000 * STEP, TARGET_FRAME_MS);

    for (int i = 0; i < 40; i++) {
        rt_pixel_budget_adapt(&budget, UNDER_BUDGET_MS);
    }
    const int grown = budget.value;
    TEST_ASSERT_EQUAL_INT(41 * STEP, grown);

    rt_pixel_budget_adapt(&budget, OVER_BUDGET_MS);
    TEST_ASSERT_LESS_THAN_INT(grown, budget.value);

    int recovery_frames = 0;
    while (budget.value < grown) {
        rt_pixel_budget_adapt(&budget, UNDER_BUDGET_MS);
        recovery_frames++;
        TEST_ASSERT_LESS_THAN_INT_MESSAGE(5, recovery_frames, "one over-budget frame took many frames to recover from");
    }
    TEST_ASSERT_EQUAL_INT(1, recovery_frames);
}

void
run_rt_pixel_budget_suite(void) {
    RUN_TEST(test_grows_by_one_step_per_under_budget_frame);
    RUN_TEST(test_growth_stops_at_max);
    RUN_TEST(test_shrink_never_drops_below_min);
    RUN_TEST(test_one_over_budget_frame_costs_one_frame_to_recover);
}

SUITE_REGISTER(run_rt_pixel_budget_suite);
