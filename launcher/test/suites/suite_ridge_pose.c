/* Portable suite: gravity into a stable, unit-length ridge pose. */

#include <math.h>
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
    TEST_ASSERT_EQUAL_INT(49, ridge_pose_column_under((ridge_vector_t){RIDGE_POSE_ONE, 0}, 100, 200, 100, 50, 100));
    TEST_ASSERT_EQUAL_INT(0, ridge_pose_column_under((ridge_vector_t){0, RIDGE_POSE_ONE}, 100, 200, 100, 0, 100));
    TEST_ASSERT_EQUAL_INT(99, ridge_pose_column_under((ridge_vector_t){0, RIDGE_POSE_ONE}, 100, 200, 100, 99, 100));
}

/* A pose `degrees` off straight down, toward the side. */
static ridge_vector_t
off_down_by(double degrees) {
    const double radians = degrees * 3.14159265358979 / 180.0;
    return (ridge_vector_t){(int32_t)lround(sin(radians) * RIDGE_POSE_ONE),
                            (int32_t)lround(cos(radians) * RIDGE_POSE_ONE)};
}

static void
test_strips_pick_the_plain_diagonal_before_the_first_paint(void) {
    TEST_ASSERT_TRUE(ridge_pose_strips_by_column(off_down_by(44.9), false, false));
    TEST_ASSERT_TRUE(ridge_pose_strips_by_column(off_down_by(44.9), false, true));
    TEST_ASSERT_FALSE(ridge_pose_strips_by_column(off_down_by(45.1), false, false));
    TEST_ASSERT_FALSE(ridge_pose_strips_by_column(off_down_by(45.1), false, true));
    TEST_ASSERT_TRUE(ridge_pose_strips_by_column((ridge_vector_t){RIDGE_POSE_ONE, RIDGE_POSE_ONE}, false, false));
}

static void
test_columns_hold_until_two_degrees_past_the_diagonal(void) {
    TEST_ASSERT_TRUE(ridge_pose_strips_by_column(off_down_by(46.9), true, true));
    TEST_ASSERT_FALSE(ridge_pose_strips_by_column(off_down_by(47.1), true, true));
    TEST_ASSERT_TRUE(ridge_pose_strips_by_column(off_down_by(46.0), true, true));
}

static void
test_rows_hold_until_two_degrees_past_the_diagonal(void) {
    TEST_ASSERT_FALSE(ridge_pose_strips_by_column(off_down_by(43.1), true, false));
    TEST_ASSERT_TRUE(ridge_pose_strips_by_column(off_down_by(42.9), true, false));
    TEST_ASSERT_FALSE(ridge_pose_strips_by_column(off_down_by(44.0), true, false));
}

static void
test_strips_ignore_which_way_the_pose_leans(void) {
    for (int sign_x = -1; sign_x <= 1; sign_x += 2) {
        for (int sign_y = -1; sign_y <= 1; sign_y += 2) {
            const ridge_vector_t lean = off_down_by(46.0);
            const ridge_vector_t pose = {sign_x * lean.down_x, sign_y * lean.down_y};
            TEST_ASSERT_TRUE(ridge_pose_strips_by_column(pose, true, true));
            TEST_ASSERT_FALSE(ridge_pose_strips_by_column(pose, true, false));
        }
    }
}

/* The thresholds are exact on the per-mille ratio, which a raw vector can
 * land on: columns are left only past it, rows are entered only past it. */
static void
test_the_hysteresis_thresholds_are_exact_on_the_ratio(void) {
    const int32_t slope = RIDGE_POSE_AXIS_SWITCH_SLOPE_PER_MILLE;
    const ridge_vector_t on_ratio_leaning = {slope, 1000};
    const ridge_vector_t on_ratio_upright = {1000, slope};
    TEST_ASSERT_TRUE(ridge_pose_strips_by_column(on_ratio_leaning, true, true));
    TEST_ASSERT_FALSE(ridge_pose_strips_by_column((ridge_vector_t){slope + 1, 1000}, true, true));
    TEST_ASSERT_FALSE(ridge_pose_strips_by_column(on_ratio_upright, true, false));
    TEST_ASSERT_TRUE(ridge_pose_strips_by_column((ridge_vector_t){1000, slope + 1}, true, false));
}

void
suite_ridge_pose(void) {
    RUN_TEST(test_easing_keeps_a_unit_vector);
    RUN_TEST(test_steady_gravity_arrives_exactly_at_level);
    RUN_TEST(test_column_under_tracks_the_pose_axis);
    RUN_TEST(test_strips_pick_the_plain_diagonal_before_the_first_paint);
    RUN_TEST(test_columns_hold_until_two_degrees_past_the_diagonal);
    RUN_TEST(test_rows_hold_until_two_degrees_past_the_diagonal);
    RUN_TEST(test_strips_ignore_which_way_the_pose_leans);
    RUN_TEST(test_the_hysteresis_thresholds_are_exact_on_the_ratio);
}

SUITE_REGISTER(suite_ridge_pose);
