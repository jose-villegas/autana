/*
 * Portable suite: motion_profile.h. The move rests at both ends, only ever
 * moves forward, its place and speed have no jumps, and a short move lowers
 * its cruise and still ends at its end.
 */

#include <math.h>

#include "testkit/suite.h"
#include "unity.h"

#include "kinematics/profile/motion_profile.h"

#define TOLERANCE             1.0E-3F
/* Sampled this often to look for jumps and backward steps. */
#define STEP_S                0.005F
/* At most this share of the cruise speed changes between two samples. */
#define SPEED_STEP_SHARE      0.01F
/* A move two ramps at the peak would overrun. */
#define SHORT_MOVE            100.0F
/* A ramp is integrated in this many midpoint steps. */
#define RAMP_STEPS            2000
/* Place and the area under the speed agree this closely, in units. */
#define AREA_TOLERANCE        0.05F
/* Either side of the join of two ramps, and how closely they agree there. */
#define JOINT_EPS_S           1.0E-4F
#define JOINT_PLACE_TOLERANCE 0.1F
#define JOINT_SPEED_TOLERANCE 1.0F
/* Seconds into a ramp-less move the position is checked at. */
#define ABRUPT_PROBE_S        1.0F

static const motion_profile_t PROFILE = {
    .start = -600.0F, .end = 500.0F, .peak_speed = 300.0F, .rest_s = 2.0F, .ramp_s = 2.5F};

static void
test_the_move_rests_at_its_start_through_the_rest_and_at_its_end_after(void) {
    for (float t = -PROFILE.rest_s; t <= PROFILE.rest_s; t += STEP_S) {
        const motion_profile_at_t at = motion_profile_at(&PROFILE, t);
        TEST_ASSERT_EQUAL_FLOAT(PROFILE.start, at.at);
        TEST_ASSERT_EQUAL_FLOAT(0.0F, at.speed);
    }
    const float duration = motion_profile_duration_s(&PROFILE);
    for (float t = duration - PROFILE.rest_s + STEP_S; t <= duration + PROFILE.rest_s; t += STEP_S) {
        const motion_profile_at_t at = motion_profile_at(&PROFILE, t);
        TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, PROFILE.end, at.at);
        TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, 0.0F, at.speed);
    }
}

/* Each step covers about its speed times its time: no jump in place or speed. */
static void
test_the_move_only_goes_forward_with_no_jump(void) {
    const float cruise = motion_profile_cruise_speed(&PROFILE);
    const float max_step = cruise * STEP_S;
    motion_profile_at_t was = motion_profile_at(&PROFILE, 0.0F);
    for (float t = STEP_S; t <= motion_profile_duration_s(&PROFILE); t += STEP_S) {
        const motion_profile_at_t at = motion_profile_at(&PROFILE, t);
        TEST_ASSERT_TRUE(at.at >= was.at);
        TEST_ASSERT_TRUE(at.at - was.at <= max_step + TOLERANCE);
        TEST_ASSERT_FLOAT_WITHIN(cruise * SPEED_STEP_SHARE, was.speed, at.speed);
        TEST_ASSERT_TRUE(at.speed <= cruise + TOLERANCE);
        was = at;
    }
}

/* Too short to reach the peak in two ramps: the cruise is the speed whose two
 * ramps exactly cover it, and it still ends at rest at its end. */
static void
test_a_short_move_lowers_its_cruise_and_still_ends_at_its_end(void) {
    motion_profile_t short_move = PROFILE;
    short_move.end = short_move.start + SHORT_MOVE;
    TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, SHORT_MOVE / PROFILE.ramp_s, motion_profile_cruise_speed(&short_move));
    TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, (2.0F * PROFILE.rest_s) + (2.0F * PROFILE.ramp_s),
                             motion_profile_duration_s(&short_move));
    TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, short_move.end,
                             motion_profile_at(&short_move, short_move.rest_s + (2.0F * PROFILE.ramp_s)).at);
    TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, PROFILE.peak_speed, motion_profile_cruise_speed(&PROFILE));
}

/* Place is the integral of the reported speed: over each ramp, the place at
 * every point of it is the start's plus the area under the speed so far, so
 * a share that is only continuous (u*u, say) is caught. */
static void
test_place_over_each_ramp_is_the_area_under_the_reported_speed(void) {
    const float up_from = PROFILE.rest_s;
    const float down_from = motion_profile_duration_s(&PROFILE) - PROFILE.rest_s - PROFILE.ramp_s;
    const float from[] = {up_from, down_from};
    for (int ramp = 0; ramp < 2; ramp++) {
        const double dt = (double)PROFILE.ramp_s / RAMP_STEPS;
        const double place_from = (double)motion_profile_at(&PROFILE, from[ramp]).at;
        double area = 0.0;
        for (int i = 0; i < RAMP_STEPS; i++) {
            const double middle = (double)from[ramp] + (((double)i + 0.5) * dt);
            area += (double)motion_profile_at(&PROFILE, (float)middle).speed * dt;
            const float t = (float)((double)from[ramp] + (((double)i + 1.0) * dt));
            TEST_ASSERT_FLOAT_WITHIN(AREA_TOLERANCE, (float)(place_from + area), motion_profile_at(&PROFILE, t).at);
        }
    }
}

/* Two ramps at the peak cover the distance exactly: no cruise, and the two
 * ramps meet at the peak with no jump. */
static void
test_a_move_exactly_two_ramps_long_has_no_cruise_and_no_jump(void) {
    motion_profile_t exact = PROFILE;
    exact.end = exact.start + (exact.peak_speed * exact.ramp_s);
    TEST_ASSERT_EQUAL_FLOAT(PROFILE.peak_speed, motion_profile_cruise_speed(&exact));
    TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, (2.0F * PROFILE.rest_s) + (2.0F * PROFILE.ramp_s),
                             motion_profile_duration_s(&exact));
    const float joint = exact.rest_s + exact.ramp_s;
    const motion_profile_at_t before = motion_profile_at(&exact, joint - JOINT_EPS_S);
    const motion_profile_at_t after = motion_profile_at(&exact, joint + JOINT_EPS_S);
    TEST_ASSERT_FLOAT_WITHIN(JOINT_PLACE_TOLERANCE, before.at, after.at);
    TEST_ASSERT_FLOAT_WITHIN(JOINT_SPEED_TOLERANCE, exact.peak_speed, before.speed);
    TEST_ASSERT_FLOAT_WITHIN(JOINT_SPEED_TOLERANCE, exact.peak_speed, after.speed);
    TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, exact.start + (0.5F * (exact.end - exact.start)),
                             motion_profile_at(&exact, joint).at);
    TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, exact.end, motion_profile_at(&exact, motion_profile_duration_s(&exact)).at);
}

/* No ramp: the move is a cruise at the peak from start to end, with no NaN. */
static void
test_a_zero_ramp_moves_at_the_peak_and_reaches_its_end(void) {
    motion_profile_t abrupt = PROFILE;
    abrupt.ramp_s = 0.0F;
    TEST_ASSERT_EQUAL_FLOAT(PROFILE.peak_speed, motion_profile_cruise_speed(&abrupt));
    const float duration = motion_profile_duration_s(&abrupt);
    TEST_ASSERT_FALSE(isnan(duration));
    TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, (2.0F * PROFILE.rest_s) + ((abrupt.end - abrupt.start) / abrupt.peak_speed),
                             duration);
    float was = abrupt.start;
    for (float t = 0.0F; t <= duration + abrupt.rest_s; t += STEP_S) {
        const motion_profile_at_t at = motion_profile_at(&abrupt, t);
        TEST_ASSERT_FALSE(isnan(at.at));
        TEST_ASSERT_FALSE(isnan(at.speed));
        TEST_ASSERT_TRUE(at.at >= was);
        was = at.at;
    }
    TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, abrupt.end, was);
    TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, abrupt.start + (abrupt.peak_speed * ABRUPT_PROBE_S),
                             motion_profile_at(&abrupt, abrupt.rest_s + ABRUPT_PROBE_S).at);
}

/* The instants the rest ends and the whole move ends are on the right side. */
static void
test_the_instants_the_rest_and_the_move_end_are_at_rest_at_start_and_end(void) {
    const motion_profile_at_t first = motion_profile_at(&PROFILE, PROFILE.rest_s);
    TEST_ASSERT_EQUAL_FLOAT(PROFILE.start, first.at);
    TEST_ASSERT_EQUAL_FLOAT(0.0F, first.speed);
    const motion_profile_at_t last = motion_profile_at(&PROFILE, motion_profile_duration_s(&PROFILE));
    TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, PROFILE.end, last.at);
    TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, 0.0F, last.speed);
    const motion_profile_at_t mid_rest =
        motion_profile_at(&PROFILE, motion_profile_duration_s(&PROFILE) - PROFILE.rest_s);
    TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, PROFILE.end, mid_rest.at);
}

/* The short move, scanned as the long one is: forward only, no jump in place
 * or speed, from rest at the start to rest at the end. */
static void
test_a_short_move_goes_only_forward_with_no_jump(void) {
    motion_profile_t short_move = PROFILE;
    short_move.end = short_move.start + SHORT_MOVE;
    const float cruise = motion_profile_cruise_speed(&short_move);
    const float max_step = cruise * STEP_S;
    motion_profile_at_t was = motion_profile_at(&short_move, 0.0F);
    for (float t = STEP_S; t <= motion_profile_duration_s(&short_move); t += STEP_S) {
        const motion_profile_at_t at = motion_profile_at(&short_move, t);
        TEST_ASSERT_TRUE(at.at >= was.at);
        TEST_ASSERT_TRUE(at.at - was.at <= max_step + TOLERANCE);
        TEST_ASSERT_FLOAT_WITHIN(cruise * SPEED_STEP_SHARE, was.speed, at.speed);
        TEST_ASSERT_TRUE(at.speed <= cruise + TOLERANCE);
        was = at;
    }
    TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, short_move.end, was.at);
}

void
suite_motion_profile(void) {
    RUN_TEST(test_the_move_rests_at_its_start_through_the_rest_and_at_its_end_after);
    RUN_TEST(test_the_move_only_goes_forward_with_no_jump);
    RUN_TEST(test_a_short_move_lowers_its_cruise_and_still_ends_at_its_end);
    RUN_TEST(test_place_over_each_ramp_is_the_area_under_the_reported_speed);
    RUN_TEST(test_a_move_exactly_two_ramps_long_has_no_cruise_and_no_jump);
    RUN_TEST(test_a_zero_ramp_moves_at_the_peak_and_reaches_its_end);
    RUN_TEST(test_the_instants_the_rest_and_the_move_end_are_at_rest_at_start_and_end);
    RUN_TEST(test_a_short_move_goes_only_forward_with_no_jump);
}

SUITE_REGISTER(suite_motion_profile);
