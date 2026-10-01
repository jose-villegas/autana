/*
 * Portable suite: anim/anim_transform.h, a node's three sampled tracks as a
 * transformf_t. The three parts land unchanged, a sampled quaternion is
 * renormalized, and the matrix built from them is the one the tracks
 * describe.
 */

#include "suites.h"
#include "unity.h"

#include "anim/anim_transform.h"

#define SLACK 1e-5F

static const float TIMES[] = {0.0F};

static anim_track_t
constant(const float* values, int width, int quaternion) {
    return (anim_track_t){TIMES, values, 1, (uint8_t)width, ANIM_LINEAR, (uint8_t)quaternion};
}

static void
test_translation_and_scale_land_unchanged(void) {
    static const float move[3] = {1.5F, -2.0F, 0.25F};
    static const float turn[4] = {0.0F, 0.0F, 0.0F, 1.0F};
    static const float size[3] = {1.0F, 0.6F, 2.0F};
    const anim_track_t m = constant(move, 3, 0);
    const anim_track_t r = constant(turn, 4, 1);
    const anim_track_t s = constant(size, 3, 0);
    const transformf_t t = anim_transform_sample(&m, &r, &s, 0.0F);

    TEST_ASSERT_EQUAL_FLOAT(1.5F, t.position.x);
    TEST_ASSERT_EQUAL_FLOAT(-2.0F, t.position.y);
    TEST_ASSERT_EQUAL_FLOAT(0.25F, t.position.z);
    TEST_ASSERT_EQUAL_FLOAT(0.6F, t.scale.y);
    TEST_ASSERT_EQUAL_FLOAT(2.0F, t.scale.z);
    TEST_ASSERT_EQUAL_FLOAT(1.0F, t.rotation.w);
}

static void
test_a_quarter_turn_about_y_takes_z_to_x_through_the_matrix(void) {
    static const float origin[3] = {0.0F, 0.0F, 0.0F};
    static const float turn[4] = {0.0F, 0.70710678F, 0.0F, 0.70710678F};
    static const float unit[3] = {1.0F, 1.0F, 1.0F};
    const anim_track_t m = constant(origin, 3, 0);
    const anim_track_t r = constant(turn, 4, 1);
    const anim_track_t s = constant(unit, 3, 0);
    transformf_t t = anim_transform_sample(&m, &r, &s, 0.0F);

    const mat4f_t matrix = transformf_matrix(&t);
    const vec3f_t got = mat4f_apply(&matrix, (vec3f_t){0.0F, 0.0F, 1.0F});
    TEST_ASSERT_FLOAT_WITHIN(SLACK, 1.0F, got.x);
    TEST_ASSERT_FLOAT_WITHIN(SLACK, 0.0F, got.z);
}

static void
test_a_sampled_quaternion_is_renormalized(void) {
    static const float origin[3] = {0.0F, 0.0F, 0.0F};
    static const float shrunk[4] = {0.0F, 0.0F, 0.0F, 0.5F};
    static const float unit[3] = {1.0F, 1.0F, 1.0F};
    const anim_track_t m = constant(origin, 3, 0);
    const anim_track_t r = constant(shrunk, 4, 1);
    const anim_track_t s = constant(unit, 3, 0);

    TEST_ASSERT_FLOAT_WITHIN(SLACK, 1.0F, anim_transform_sample(&m, &r, &s, 0.0F).rotation.w);
}

static void
test_a_rotation_between_two_keys_is_the_halfway_turn(void) {
    static const float times[] = {0.0F, 1.0F};
    static const float turns[8] = {0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.70710678F, 0.0F, 0.70710678F};
    static const float origin[3] = {0.0F, 0.0F, 0.0F};
    static const float unit[3] = {1.0F, 1.0F, 1.0F};
    const anim_track_t m = constant(origin, 3, 0);
    const anim_track_t r = {times, turns, 2, 4, ANIM_LINEAR, 1};
    const anim_track_t s = constant(unit, 3, 0);

    const transformf_t t = anim_transform_sample(&m, &r, &s, 0.5F);
    const vec3f_t z = quatf_rotate(t.rotation, (vec3f_t){0.0F, 0.0F, 1.0F});
    TEST_ASSERT_FLOAT_WITHIN(1e-4F, 0.70710678F, z.x);
    TEST_ASSERT_FLOAT_WITHIN(1e-4F, 0.70710678F, z.z);
}

void
suite_anim_transform(void) {
    RUN_TEST(test_translation_and_scale_land_unchanged);
    RUN_TEST(test_a_quarter_turn_about_y_takes_z_to_x_through_the_matrix);
    RUN_TEST(test_a_sampled_quaternion_is_renormalized);
    RUN_TEST(test_a_rotation_between_two_keys_is_the_halfway_turn);
}

SUITE_REGISTER(suite_anim_transform);
