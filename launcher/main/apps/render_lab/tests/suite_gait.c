/*
 * Portable suite: locomotion/gait.h. A speed picks the step whose range holds
 * it, the rate keeps the feet on the ground inside the clamp, and a held step
 * survives a speed inside its band.
 */
#include "suites.h"
#include "testkit/suite.h"
#include "unity.h"

#include "apps/render_lab/locomotion/gait.h"

#define TOLERANCE 1.0E-5F
#define NO_STEP   (-1)
/* A speed this far past a boundary is clearly on one side of it. */
#define PAST      (TOLERANCE * 10.0F)

static const gait_step_t STEPS[] = {
    {.clip = 10, .ground_mps = 0.0F}, /* still */
    {.clip = 11, .ground_mps = 1.0F},
    {.clip = 12, .ground_mps = 2.0F},
    {.clip = 13, .ground_mps = 5.0F},
};
#define STEP_COUNT ((int)(sizeof STEPS / sizeof STEPS[0]))

static const gait_t GAIT = {
    .steps = STEPS,
    .count = STEP_COUNT,
    .rate_min = 0.75F,
    .rate_max = 1.4F,
    .band = 0.2F,
    .blend_ms = 300,
};

static gait_pick_t
fresh(float speed) {
    return gait_pick(&GAIT, (gait_pick_t){NO_STEP, 1.0F}, speed);
}

/* The speed halfway between two steps' ground speeds: where a fresh pick changes step. */
static float
boundary(int below) {
    return 0.5F * (STEPS[below].ground_mps + STEPS[below + 1].ground_mps);
}

/* Steps listed in any order come out slowest first, each keeping its own clip. */
static void
test_sorting_puts_steps_slowest_first_with_their_clips(void) {
    gait_step_t steps[] = {STEPS[2], STEPS[0], STEPS[3], STEPS[1]};
    gait_sort(steps, STEP_COUNT);
    for (int i = 0; i < STEP_COUNT; i++) {
        TEST_ASSERT_EQUAL_INT(STEPS[i].clip, steps[i].clip);
        TEST_ASSERT_EQUAL_FLOAT(STEPS[i].ground_mps, steps[i].ground_mps);
    }
}

static void
test_each_steps_own_speed_picks_it_at_rate_one(void) {
    for (int i = 0; i < STEP_COUNT; i++) {
        const gait_pick_t p = fresh(STEPS[i].ground_mps);
        TEST_ASSERT_EQUAL_INT(i, p.step);
        TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, 1.0F, p.rate);
    }
}

static void
test_a_fresh_pick_changes_step_at_the_midpoint(void) {
    for (int i = 0; i + 1 < STEP_COUNT; i++) {
        TEST_ASSERT_EQUAL_INT(i, fresh(boundary(i) - PAST).step);
        TEST_ASSERT_EQUAL_INT(i + 1, fresh(boundary(i) + PAST).step);
    }
}

static void
test_the_rate_matches_the_ground_speed_inside_the_clamp(void) {
    const float speed = 2.3F; /* nearest the 2 m/s step, inside its rate range */
    const gait_pick_t p = fresh(speed);
    TEST_ASSERT_EQUAL_INT(2, p.step);
    TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, speed, p.rate * STEPS[2].ground_mps);
    const gait_pick_t fast = fresh(1000.0F);
    TEST_ASSERT_EQUAL_INT(STEP_COUNT - 1, fast.step);
    TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, GAIT.rate_max, fast.rate);
    const gait_pick_t slow = fresh(boundary(0) + PAST);
    TEST_ASSERT_EQUAL_INT(1, slow.step);
    TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, GAIT.rate_min, slow.rate);
}

static void
test_a_still_step_plays_at_rate_one(void) {
    const gait_pick_t p = fresh(0.1F);
    TEST_ASSERT_EQUAL_INT(0, p.step);
    TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, 1.0F, p.rate);
}

static void
test_a_held_step_survives_inside_its_band_and_leaves_past_it(void) {
    const gait_pick_t walking = fresh(STEPS[1].ground_mps);
    const float edge = boundary(1);
    TEST_ASSERT_EQUAL_INT(1, gait_pick(&GAIT, walking, edge + (GAIT.band * 0.5F)).step);
    TEST_ASSERT_EQUAL_INT(2, gait_pick(&GAIT, walking, edge + (GAIT.band * 1.5F)).step);
    const gait_pick_t trotting = fresh(STEPS[2].ground_mps);
    TEST_ASSERT_EQUAL_INT(2, gait_pick(&GAIT, trotting, edge - (GAIT.band * 0.5F)).step);
    TEST_ASSERT_EQUAL_INT(1, gait_pick(&GAIT, trotting, edge - (GAIT.band * 1.5F)).step);
}

void
run_gait_suite(void) {
    RUN_TEST(test_sorting_puts_steps_slowest_first_with_their_clips);
    RUN_TEST(test_each_steps_own_speed_picks_it_at_rate_one);
    RUN_TEST(test_a_fresh_pick_changes_step_at_the_midpoint);
    RUN_TEST(test_the_rate_matches_the_ground_speed_inside_the_clamp);
    RUN_TEST(test_a_still_step_plays_at_rate_one);
    RUN_TEST(test_a_held_step_survives_inside_its_band_and_leaves_past_it);
}

SUITE_REGISTER(run_gait_suite);
