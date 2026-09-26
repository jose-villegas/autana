/* Portable suite: gravity into a stable, unit-length ridge pose. */

#include <stdint.h>

#include "suites.h"
#include "unity.h"

#include "ui/ridge_pose.h"

#define TAU_MS         200
#define HOLD_MS        500u
#define STEADY_STEP    50
#define STEADY_HOLD_MS 300u
#define REDRAW_STEP    100

static ridge_vector_t
normalised(int32_t x, int32_t y) {
    const int64_t length = ridge_pose_isqrt((uint32_t)(x * x + y * y));
    return length == 0 ? (ridge_vector_t){RIDGE_POSE_ONE, 0}
                       : (ridge_vector_t){(int32_t)((int64_t)x * RIDGE_POSE_ONE / length),
                                          (int32_t)((int64_t)y * RIDGE_POSE_ONE / length)};
}

static void
test_easing_keeps_a_unit_vector(void) {
    ridge_pose_t pose = {.pose = (ridge_vector_t){RIDGE_POSE_ONE, 0}};
    for (int frame = 0; frame < 100; frame++) {
        ridge_pose_ease(&pose, normalised(-9000, 13000), 16, TAU_MS);
        const int64_t length2 =
            (int64_t)pose.pose.down_x * pose.pose.down_x + (int64_t)pose.pose.down_y * pose.pose.down_y;
        TEST_ASSERT_INT_WITHIN(4, RIDGE_POSE_ONE, (int)ridge_pose_isqrt((uint32_t)length2));
    }
}

static void
test_steady_gravity_arrives_exactly_at_level(void) {
    const ridge_vector_t level = normalised(0, RIDGE_POSE_ONE);
    ridge_pose_t pose = {.pose = (ridge_vector_t){RIDGE_POSE_ONE, 0},
                         .level = level,
                         .steady_level = level,
                         .steady_ms = STEADY_HOLD_MS};
    const ridge_pose_params_t params = {.boot_pose = pose.pose,
                                        .hold_ms = HOLD_MS,
                                        .tau_ms = TAU_MS,
                                        .steady_step = STEADY_STEP,
                                        .steady_hold_ms = STEADY_HOLD_MS,
                                        .redraw_step = REDRAW_STEP};
    for (int frame = 0; frame < 100; frame++) {
        ridge_pose_advance(&pose, &params, 16, HOLD_MS + STEADY_HOLD_MS + 1);
    }
    TEST_ASSERT_EQUAL_INT32(level.down_x, pose.pose.down_x);
    TEST_ASSERT_EQUAL_INT32(level.down_y, pose.pose.down_y);
}

static void
test_column_under_tracks_the_pose_axis(void) {
    TEST_ASSERT_EQUAL_INT(49, ridge_pose_column_under((ridge_vector_t){RIDGE_POSE_ONE, 0}, 100, 200, 100, 50, 0));
    TEST_ASSERT_EQUAL_INT(0, ridge_pose_column_under((ridge_vector_t){0, RIDGE_POSE_ONE}, 100, 200, 100, 0, 100));
    TEST_ASSERT_EQUAL_INT(99, ridge_pose_column_under((ridge_vector_t){0, RIDGE_POSE_ONE}, 100, 200, 100, 99, 100));
}

void
suite_ridge_pose(void) {
    RUN_TEST(test_easing_keeps_a_unit_vector);
    RUN_TEST(test_steady_gravity_arrives_exactly_at_level);
    RUN_TEST(test_column_under_tracks_the_pose_axis);
}

SUITE_REGISTER(suite_ridge_pose);
