/*
 * Portable suite: capybara_sponza/capybara_run.h. The run rests at both ends,
 * only ever moves forward, its place and speed have no jumps, a short run
 * lowers its cruise, and the camera faces the runner while it is still and
 * stands at its back at the cruise speed.
 */
#include "suites.h"
#include "testkit/suite.h"
#include "unity.h"

#include "apps/render_lab/capybara_sponza/capybara_run.h"
#include "math/scalar/math_const.h"

#define TOLERANCE        1.0E-3F
/* Sampled this often to look for jumps and backward steps. */
#define STEP_MS          5U
/* At most this share of the cruise speed changes between two samples. */
#define SPEED_STEP_SHARE 0.01F
/* A run two ramps at the peak would overrun. */
#define SHORT_RUN        100.0F
#define MS_PER_S         1000.0F

static const capybara_run_t RUN = {
    .start = -600.0F, .end = 500.0F, .peak_speed = 300.0F, .idle_ms = 2000U, .ramp_ms = 2500U};

static void
test_the_run_rests_at_its_start_through_the_idle_and_at_its_end_after(void) {
    for (uint32_t t = 0; t <= RUN.idle_ms; t += STEP_MS) {
        const capybara_run_at_t at = capybara_run_at(&RUN, t);
        TEST_ASSERT_EQUAL_FLOAT(RUN.start, at.at);
        TEST_ASSERT_EQUAL_FLOAT(0.0F, at.speed);
    }
    const uint32_t duration = capybara_run_duration_ms(&RUN);
    for (uint32_t t = duration - RUN.idle_ms + STEP_MS; t <= duration + RUN.idle_ms; t += STEP_MS) {
        const capybara_run_at_t at = capybara_run_at(&RUN, t);
        TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, RUN.end, at.at);
        TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, 0.0F, at.speed);
    }
}

/* Each step covers about its speed times its time: no jump in place or speed. */
static void
test_the_run_only_moves_forward_with_no_jump(void) {
    const float max_step = capybara_run_cruise_speed(&RUN) * (float)STEP_MS / MS_PER_S;
    const float max_speed_step = capybara_run_cruise_speed(&RUN) * SPEED_STEP_SHARE;
    capybara_run_at_t was = capybara_run_at(&RUN, 0);
    for (uint32_t t = STEP_MS; t <= capybara_run_duration_ms(&RUN); t += STEP_MS) {
        const capybara_run_at_t at = capybara_run_at(&RUN, t);
        TEST_ASSERT_TRUE(at.at >= was.at);
        TEST_ASSERT_TRUE(at.at - was.at <= max_step + TOLERANCE);
        TEST_ASSERT_FLOAT_WITHIN(max_speed_step, was.speed, at.speed);
        TEST_ASSERT_TRUE(at.speed <= capybara_run_cruise_speed(&RUN) + TOLERANCE);
        was = at;
    }
}

/* Too short to reach the peak in two ramps: the cruise is the speed whose two
 * ramps exactly cover it, and it still ends at rest at its end. */
static void
test_a_short_run_lowers_its_cruise_and_still_ends_at_its_end(void) {
    capybara_run_t short_run = RUN;
    short_run.end = short_run.start + SHORT_RUN;
    TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, SHORT_RUN / ((float)RUN.ramp_ms / MS_PER_S),
                             capybara_run_cruise_speed(&short_run));
    TEST_ASSERT_EQUAL_UINT32((2U * RUN.idle_ms) + (2U * RUN.ramp_ms), capybara_run_duration_ms(&short_run));
    TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, short_run.end,
                             capybara_run_at(&short_run, short_run.idle_ms + RUN.ramp_ms * 2U).at);
    TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, RUN.peak_speed, capybara_run_cruise_speed(&RUN));
}

static void
test_the_camera_faces_a_still_runner_and_stands_behind_at_the_cruise(void) {
    TEST_ASSERT_EQUAL_FLOAT(0.0F, capybara_run_camera_angle(&RUN, 0.0F));
    TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, MATH_PI, capybara_run_camera_angle(&RUN, capybara_run_cruise_speed(&RUN)));
    TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, 0.5F * MATH_PI,
                             capybara_run_camera_angle(&RUN, 0.5F * capybara_run_cruise_speed(&RUN)));
}

void
run_capybara_run_suite(void) {
    RUN_TEST(test_the_run_rests_at_its_start_through_the_idle_and_at_its_end_after);
    RUN_TEST(test_the_run_only_moves_forward_with_no_jump);
    RUN_TEST(test_a_short_run_lowers_its_cruise_and_still_ends_at_its_end);
    RUN_TEST(test_the_camera_faces_a_still_runner_and_stands_behind_at_the_cruise);
}

SUITE_REGISTER(run_capybara_run_suite);
