/*
 * Portable suite: render/r3d_trs.h, a track's sampled translation,
 * quaternion and scale as a transform_t. The three parts land unchanged, the
 * quaternion is renormalized, and the matrix built from them is the one the
 * track describes.
 */

#include "suites.h"
#include "unity.h"

#include "render/r3d_trs.h"

#define SLACK 1e-5F

static void
test_translation_and_scale_land_unchanged(void) {
    const float move[3] = {1.5F, -2.0F, 0.25F};
    const float size[3] = {1.0F, 0.6F, 2.0F};
    const float identity[4] = {0.0F, 0.0F, 0.0F, 1.0F};
    const transform_t t = r3d_trs_to_transform(move, identity, size);

    TEST_ASSERT_EQUAL_FLOAT(1.5F, t.position.x);
    TEST_ASSERT_EQUAL_FLOAT(-2.0F, t.position.y);
    TEST_ASSERT_EQUAL_FLOAT(0.25F, t.position.z);
    TEST_ASSERT_EQUAL_FLOAT(1.0F, t.scale.x);
    TEST_ASSERT_EQUAL_FLOAT(0.6F, t.scale.y);
    TEST_ASSERT_EQUAL_FLOAT(2.0F, t.scale.z);
    TEST_ASSERT_EQUAL_FLOAT(0.0F, t.rotation.x);
    TEST_ASSERT_EQUAL_FLOAT(1.0F, t.rotation.w);
}

static void
test_a_quarter_turn_about_y_takes_z_to_x(void) {
    const float origin[3] = {0.0F, 0.0F, 0.0F};
    const float unit[3] = {1.0F, 1.0F, 1.0F};
    const float quarter_y[4] = {0.0F, 0.70710678F, 0.0F, 0.70710678F};
    transform_t t = r3d_trs_to_transform(origin, quarter_y, unit);

    const vec3_t got = quat_rotate(t.rotation, (vec3_t){0.0F, 0.0F, 1.0F});
    TEST_ASSERT_FLOAT_WITHIN(SLACK, 1.0F, got.x);
    TEST_ASSERT_FLOAT_WITHIN(SLACK, 0.0F, got.z);

    const mat4_t m = transform_matrix(&t);
    const vec3_t via_matrix = mat4_apply(&m, (vec3_t){0.0F, 0.0F, 1.0F});
    TEST_ASSERT_FLOAT_WITHIN(SLACK, 1.0F, via_matrix.x);
}

static void
test_a_sampled_quaternion_between_keys_is_renormalized(void) {
    const float origin[3] = {0.0F, 0.0F, 0.0F};
    const float unit[3] = {1.0F, 1.0F, 1.0F};
    const float shrunk[4] = {0.0F, 0.0F, 0.0F, 0.5F};
    const transform_t t = r3d_trs_to_transform(origin, shrunk, unit);

    TEST_ASSERT_FLOAT_WITHIN(SLACK, 1.0F, t.rotation.w);
}

void
suite_r3d_trs(void) {
    RUN_TEST(test_translation_and_scale_land_unchanged);
    RUN_TEST(test_a_quarter_turn_about_y_takes_z_to_x);
    RUN_TEST(test_a_sampled_quaternion_between_keys_is_renormalized);
}

SUITE_REGISTER(suite_r3d_trs);
