/*
 * Portable suite: anim/anim_track, what a track returns at, before and
 * after its keys, whatever it will later drive. Every track is built here.
 * That the baker's output and the Python sampler agree with it is
 * tools/tests/test_anim_bake.py.
 */

#include <math.h>
#include <stdint.h>

#include "suites.h"
#include "unity.h"

#include "anim/anim_track.h"

#define EPSILON 1e-5F

static const float TIMES[] = {0.0F, 1.0F, 3.0F};

/* A clip of this one track, as a baked animation of it alone would be. */
static void
sample_at(const anim_track_t* track, uint32_t t_ms, anim_wrap_t wrap, float out[ANIM_WIDTH_MAX]) {
    const anim_clip_t clip = {(uint32_t)((track->times[track->count - 1] * 1000.0F) + 0.5F)};
    anim_track_sample(track, anim_clip_seconds(&clip, t_ms, wrap), out);
}

static anim_track_t
scalar_track(const float* values, anim_interp_t interp) {
    return (anim_track_t){TIMES, values, 3, 1, (uint8_t)interp, 0};
}

static void
test_a_linear_track_lerps_between_its_keys(void) {
    const float values[] = {0.0F, 10.0F, 30.0F};
    const anim_track_t track = scalar_track(values, ANIM_LINEAR);
    float out[ANIM_WIDTH_MAX];
    sample_at(&track, 500, ANIM_CLAMP, out);
    TEST_ASSERT_FLOAT_WITHIN(EPSILON, 5.0F, out[0]);
    sample_at(&track, 2000, ANIM_CLAMP, out);
    TEST_ASSERT_FLOAT_WITHIN(EPSILON, 20.0F, out[0]);
}

static void
test_a_step_track_holds_a_value_until_the_next_key(void) {
    const float values[] = {1.0F, 2.0F, 3.0F};
    const anim_track_t track = scalar_track(values, ANIM_STEP);
    float out[ANIM_WIDTH_MAX];
    sample_at(&track, 999, ANIM_CLAMP, out);
    TEST_ASSERT_EQUAL_FLOAT(1.0F, out[0]);
    sample_at(&track, 1000, ANIM_CLAMP, out);
    TEST_ASSERT_EQUAL_FLOAT(2.0F, out[0]);
}

static void
test_a_clamped_track_holds_its_ends(void) {
    const float values[] = {4.0F, 10.0F, 30.0F};
    const anim_track_t track = scalar_track(values, ANIM_LINEAR);
    float out[ANIM_WIDTH_MAX];
    sample_at(&track, 3000, ANIM_CLAMP, out);
    TEST_ASSERT_EQUAL_FLOAT(30.0F, out[0]);
    sample_at(&track, 90000, ANIM_CLAMP, out);
    TEST_ASSERT_EQUAL_FLOAT(30.0F, out[0]);
    sample_at(&track, 0, ANIM_CLAMP, out);
    TEST_ASSERT_EQUAL_FLOAT(4.0F, out[0]);
}

static void
test_a_looped_track_wraps_at_its_last_key(void) {
    const float values[] = {0.0F, 10.0F, 30.0F};
    const anim_track_t track = scalar_track(values, ANIM_LINEAR);
    float out[ANIM_WIDTH_MAX];
    sample_at(&track, 3500, ANIM_LOOP, out);
    TEST_ASSERT_FLOAT_WITHIN(EPSILON, 5.0F, out[0]);
    sample_at(&track, 3000 * 7 + 2000, ANIM_LOOP, out);
    TEST_ASSERT_FLOAT_WITHIN(EPSILON * 20.0F, 20.0F, out[0]);
}

static void
test_a_track_starting_late_holds_its_first_value_until_its_time(void) {
    const float times[] = {2.0F, 4.0F};
    const float values[] = {1.0F, 9.0F};
    const anim_track_t track = {times, values, 2, 1, ANIM_LINEAR, 0};
    float out[ANIM_WIDTH_MAX];
    sample_at(&track, 1000, ANIM_CLAMP, out);
    TEST_ASSERT_EQUAL_FLOAT(1.0F, out[0]);
    sample_at(&track, 3000, ANIM_CLAMP, out);
    TEST_ASSERT_FLOAT_WITHIN(EPSILON, 5.0F, out[0]);
}

/* Two tracks of one animation, one over 0..2 s and one over 1..4 s: the clip
 * is 4 s, both loop at it, and both read the same clip second. */
static void
test_tracks_with_different_key_ranges_stay_in_step_looped_and_clamped(void) {
    const float early_times[] = {0.0F, 2.0F};
    const float early_values[] = {0.0F, 20.0F};
    const float late_times[] = {1.0F, 4.0F};
    const float late_values[] = {100.0F, 130.0F};
    const anim_track_t early = {early_times, early_values, 2, 1, ANIM_LINEAR, 0};
    const anim_track_t late = {late_times, late_values, 2, 1, ANIM_LINEAR, 0};
    const anim_clip_t clip = {4000};
    float a[ANIM_WIDTH_MAX];
    float b[ANIM_WIDTH_MAX];
    /* 5 s into a looping 4 s clip is 1 s: the early track is at 10, the late one at its start. */
    float seconds = anim_clip_seconds(&clip, 5000, ANIM_LOOP);
    anim_track_sample(&early, seconds, a);
    anim_track_sample(&late, seconds, b);
    TEST_ASSERT_FLOAT_WITHIN(EPSILON, 10.0F, a[0]);
    TEST_ASSERT_FLOAT_WITHIN(EPSILON, 100.0F, b[0]);
    /* Clamped, 9 s is the clip's end: the early track has held 20 since 2 s. */
    seconds = anim_clip_seconds(&clip, 9000, ANIM_CLAMP);
    anim_track_sample(&early, seconds, a);
    anim_track_sample(&late, seconds, b);
    TEST_ASSERT_EQUAL_FLOAT(20.0F, a[0]);
    TEST_ASSERT_EQUAL_FLOAT(130.0F, b[0]);
}

static void
test_a_long_run_keeps_millisecond_resolution(void) {
    const anim_clip_t clip = {3000};
    /* 3 999 999 001 ms is 1 ms into a lap: a float of milliseconds would be off by hundreds. */
    const float seconds = anim_clip_seconds(&clip, 3999999001U, ANIM_LOOP);
    TEST_ASSERT_FLOAT_WITHIN(1e-6F, 0.001F, seconds);
}

static void
test_a_single_key_track_is_that_value_always(void) {
    const float times[] = {0.0F};
    const float values[] = {7.0F};
    const anim_track_t track = {times, values, 1, 1, ANIM_LINEAR, 0};
    float out[ANIM_WIDTH_MAX];
    sample_at(&track, 12345, ANIM_LOOP, out);
    TEST_ASSERT_EQUAL_FLOAT(7.0F, out[0]);
}

static void
test_a_cubic_track_leaves_and_arrives_at_its_tangents(void) {
    /* Keys (0, tangent 0) and (1, tangent 0) make the smoothstep: the
     * midpoint is half way, and a quarter in is 5/32 of the way. */
    const float times[] = {0.0F, 1.0F};
    const float values[] = {0.0F, 0.0F, 0.0F, 0.0F, 8.0F, 0.0F};
    const anim_track_t track = {times, values, 2, 1, ANIM_CUBIC, 0};
    float out[ANIM_WIDTH_MAX];
    sample_at(&track, 500, ANIM_CLAMP, out);
    TEST_ASSERT_FLOAT_WITHIN(EPSILON, 4.0F, out[0]);
    sample_at(&track, 250, ANIM_CLAMP, out);
    TEST_ASSERT_FLOAT_WITHIN(EPSILON, 8.0F * 0.15625F, out[0]);
}

static void
test_a_cubic_tangent_is_per_second_so_a_longer_segment_covers_the_same_shape(void) {
    /* A value going 0 to 6 with slope 3 per second both ways, over 2 s, is a line. */
    const float times[] = {0.0F, 2.0F};
    const float values[] = {3.0F, 0.0F, 3.0F, 3.0F, 6.0F, 3.0F};
    const anim_track_t track = {times, values, 2, 1, ANIM_CUBIC, 0};
    float out[ANIM_WIDTH_MAX];
    sample_at(&track, 500, ANIM_CLAMP, out);
    TEST_ASSERT_FLOAT_WITHIN(EPSILON, 1.5F, out[0]);
}

static void
test_a_cubic_track_passes_through_every_key(void) {
    /* Three keys, each with its own in and out tangent. */
    const float values[] = {0.0F, 1.0F, 5.0F, 4.0F, 2.0F, -3.0F, 0.5F, 9.0F, 0.0F};
    const anim_track_t track = {TIMES, values, 3, 1, ANIM_CUBIC, 0};
    float out[ANIM_WIDTH_MAX];
    for (int key = 0; key < 3; key++) {
        sample_at(&track, (uint32_t)(TIMES[key] * 1000.0F), ANIM_CLAMP, out);
        TEST_ASSERT_FLOAT_WITHIN(EPSILON, values[(key * 3) + 1], out[0]);
    }
}

static void
test_a_closed_loop_is_continuous_across_its_wrap(void) {
    /* The last key repeats the first, with matching tangents: no step at the seam. */
    const float times[] = {0.0F, 1.5F, 3.0F};
    const float values[] = {2.0F, 0.0F, 2.0F, 2.0F, 6.0F, -2.0F, 2.0F, 0.0F, 2.0F};
    const anim_track_t track = {times, values, 3, 1, ANIM_CUBIC, 0};
    float before[ANIM_WIDTH_MAX];
    float after[ANIM_WIDTH_MAX];
    sample_at(&track, 2999, ANIM_LOOP, before);
    sample_at(&track, 3001, ANIM_LOOP, after);
    TEST_ASSERT_FLOAT_WITHIN(0.02F, before[0], after[0]);
}

static void
test_a_quaternion_track_turns_the_short_way_at_constant_speed(void) {
    const float half = sqrtf(0.5F);
    /* Identity to 90 degrees about z, the second key stored negated. */
    const float times[] = {0.0F, 1.0F};
    const float values[] = {0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, -half, -half};
    const anim_track_t track = {times, values, 2, 4, ANIM_LINEAR, 1};
    float q[ANIM_WIDTH_MAX];
    sample_at(&track, 500, ANIM_CLAMP, q);
    const float eighth_turn_sine = sinf(0.3926991F); /* sine of 22.5 degrees, half the 45 the slerp is at */
    TEST_ASSERT_FLOAT_WITHIN(EPSILON, eighth_turn_sine, fabsf(q[2]));
    TEST_ASSERT_FLOAT_WITHIN(EPSILON, 1.0F, sqrtf((q[0] * q[0]) + (q[1] * q[1]) + (q[2] * q[2]) + (q[3] * q[3])));
}

static void
test_rotating_by_a_quaternion_turns_a_vector(void) {
    const float half = sqrtf(0.5F);
    const float about_y[4] = {0.0F, half, 0.0F, half}; /* 90 degrees about +y */
    const float ahead[3] = {0.0F, 0.0F, -1.0F};
    float out[3];
    anim_quat_rotate(about_y, ahead, out);
    TEST_ASSERT_FLOAT_WITHIN(EPSILON, -1.0F, out[0]);
    TEST_ASSERT_FLOAT_WITHIN(EPSILON, 0.0F, out[1]);
    TEST_ASSERT_FLOAT_WITHIN(EPSILON, 0.0F, out[2]);
}

static void
test_a_track_of_any_width_fills_that_many_values(void) {
    const float times[] = {0.0F, 1.0F};
    const float values[] = {0.0F, 10.0F, 20.0F, 30.0F, 1.0F, 11.0F, 21.0F, 31.0F};
    const anim_track_t track = {times, values, 2, 4, ANIM_LINEAR, 0};
    float out[ANIM_WIDTH_MAX];
    sample_at(&track, 500, ANIM_CLAMP, out);
    for (int i = 0; i < 4; i++) {
        TEST_ASSERT_FLOAT_WITHIN(EPSILON, (10.0F * (float)i) + 0.5F, out[i]);
    }
}

void
suite_anim_track(void) {
    RUN_TEST(test_a_linear_track_lerps_between_its_keys);
    RUN_TEST(test_a_step_track_holds_a_value_until_the_next_key);
    RUN_TEST(test_a_clamped_track_holds_its_ends);
    RUN_TEST(test_a_looped_track_wraps_at_its_last_key);
    RUN_TEST(test_a_track_starting_late_holds_its_first_value_until_its_time);
    RUN_TEST(test_tracks_with_different_key_ranges_stay_in_step_looped_and_clamped);
    RUN_TEST(test_a_long_run_keeps_millisecond_resolution);
    RUN_TEST(test_a_single_key_track_is_that_value_always);
    RUN_TEST(test_a_cubic_track_leaves_and_arrives_at_its_tangents);
    RUN_TEST(test_a_cubic_tangent_is_per_second_so_a_longer_segment_covers_the_same_shape);
    RUN_TEST(test_a_cubic_track_passes_through_every_key);
    RUN_TEST(test_a_closed_loop_is_continuous_across_its_wrap);
    RUN_TEST(test_a_quaternion_track_turns_the_short_way_at_constant_speed);
    RUN_TEST(test_rotating_by_a_quaternion_turns_a_vector);
    RUN_TEST(test_a_track_of_any_width_fills_that_many_values);
}

SUITE_REGISTER(suite_anim_track);
