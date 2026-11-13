/*
 * Portable suite: motion_profile.h. The move rests at both ends, only ever
 * moves forward, its place and speed have no jumps, and a short move lowers
 * its cruise and still ends at its end.
 */

#include "testkit/suite.h"
#include "unity.h"

#include "kinematics/profile/motion_profile.h"

#define TOLERANCE        1.0E-3F
/* Sampled this often to look for jumps and backward steps. */
#define STEP_S           0.005F
/* At most this share of the cruise speed changes between two samples. */
#define SPEED_STEP_SHARE 0.01F
/* A move two ramps at the peak would overrun. */
#define SHORT_MOVE       100.0F

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

void
suite_motion_profile(void) {
    RUN_TEST(test_the_move_rests_at_its_start_through_the_rest_and_at_its_end_after);
    RUN_TEST(test_the_move_only_goes_forward_with_no_jump);
    RUN_TEST(test_a_short_move_lowers_its_cruise_and_still_ends_at_its_end);
}

SUITE_REGISTER(suite_motion_profile);
