/*
 * Portable suite: ridge_pose.h - gravity into a pose, eased and snapped
 * level, and where a screen point lies along the line.
 */

#include <stdbool.h>
#include <stdint.h>

#include "suites.h"
#include "unity.h"

#include "ui/ridge_pose.h"
#include "util/trig.h"

/* This suite's own fixture: any reasonable values, since every threshold
 * here is a parameter the maths takes rather than a shipped tunable. */
#define TAU_MS         200
#define HOLD_MS        500u
#define STEADY_STEP    50
#define STEADY_HOLD_MS 300u
#define REDRAW_STEP    100
#define DT_MS          16u

static gfx_glow_pose_t
normalized_pose(int32_t dx, int32_t dy) {
    const int64_t len = (int64_t)gfx_glow_isqrt((uint32_t)(dx * dx + dy * dy));
    if (len == 0) {
        return (gfx_glow_pose_t){GFX_GLOW_POSE_ONE, 0};
    }
    return (gfx_glow_pose_t){(int32_t)((int64_t)dx * GFX_GLOW_POSE_ONE / len),
                             (int32_t)((int64_t)dy * GFX_GLOW_POSE_ONE / len)};
}

static int32_t
pose_length(gfx_glow_pose_t p) {
    const int64_t len2 = (int64_t)p.down_x * p.down_x + (int64_t)p.down_y * p.down_y;
    return (int32_t)gfx_glow_isqrt((uint32_t)len2);
}

static void
test_an_eased_pose_stays_a_unit_vector_over_a_long_turn(void) {
    ridge_pose_t rp = {.pose = normalized_pose(3, 16383)};
    for (int leg = 0; leg < 8; leg++) {
        const uint16_t phase = (uint16_t)(leg * 65536u / 8);
        const gfx_glow_pose_t target = normalized_pose(trig_cos(phase), trig_sin(phase));
        for (int frame = 0; frame < 60; frame++) {
            ridge_pose_ease(&rp, target, DT_MS, TAU_MS);
            TEST_ASSERT_INT_WITHIN(4, GFX_GLOW_POSE_ONE, pose_length(rp.pose));
        }
    }
}

static void
test_a_quarter_turn_arrives_and_stops(void) {
    const gfx_glow_pose_t target = (gfx_glow_pose_t){0, GFX_GLOW_POSE_ONE};
    ridge_pose_t rp = {
        .pose = (gfx_glow_pose_t){GFX_GLOW_POSE_ONE, 0},
        .level = target,
        .steady_level = target,
        .steady_ms = STEADY_HOLD_MS,
    };
    bool arrived = false;
    int frame = 0;
    for (; frame < 1000 && !arrived; frame++) {
        arrived = ridge_pose_advance(&rp, DT_MS, HOLD_MS + 1, HOLD_MS, target, TAU_MS, STEADY_STEP, STEADY_HOLD_MS,
                                     REDRAW_STEP);
    }
    TEST_ASSERT_TRUE_MESSAGE(arrived, "never arrived");
    TEST_ASSERT_GREATER_THAN_INT(3, frame);
    TEST_ASSERT_EQUAL_INT32(target.down_x, rp.pose.down_x);
    TEST_ASSERT_EQUAL_INT32(target.down_y, rp.pose.down_y);

    for (int i = 0; i < 20; i++) {
        ridge_pose_advance(&rp, DT_MS, HOLD_MS + 1, HOLD_MS, target, TAU_MS, STEADY_STEP, STEADY_HOLD_MS, REDRAW_STEP);
        TEST_ASSERT_EQUAL_INT32(target.down_x, rp.pose.down_x);
        TEST_ASSERT_EQUAL_INT32(target.down_y, rp.pose.down_y);
    }
}

static void
assert_half_turn_between_exactly_opposed_poses_arrives(gfx_glow_pose_t start, gfx_glow_pose_t target) {
    ridge_pose_t rp = {.pose = start, .level = target, .steady_level = target, .steady_ms = STEADY_HOLD_MS};
    bool moved = false;
    bool arrived = false;
    for (int frame = 0; frame < 2000 && !arrived; frame++) {
        arrived = ridge_pose_advance(&rp, DT_MS, HOLD_MS + 1, HOLD_MS, target, TAU_MS, STEADY_STEP, STEADY_HOLD_MS,
                                     REDRAW_STEP);
        moved = moved || rp.pose.down_x != start.down_x || rp.pose.down_y != start.down_y;
    }
    TEST_ASSERT_TRUE_MESSAGE(moved, "the opposed pose never turned");
    TEST_ASSERT_TRUE_MESSAGE(arrived, "the opposed pose never arrived");
    TEST_ASSERT_EQUAL_INT32(target.down_x, rp.pose.down_x);
    TEST_ASSERT_EQUAL_INT32(target.down_y, rp.pose.down_y);
}

static void
test_a_half_turn_between_exactly_opposed_poses_turns_and_arrives(void) {
    assert_half_turn_between_exactly_opposed_poses_arrives((gfx_glow_pose_t){GFX_GLOW_POSE_ONE, 0},
                                                           (gfx_glow_pose_t){-GFX_GLOW_POSE_ONE, 0});
    assert_half_turn_between_exactly_opposed_poses_arrives((gfx_glow_pose_t){0, GFX_GLOW_POSE_ONE},
                                                           (gfx_glow_pose_t){0, -GFX_GLOW_POSE_ONE});
}

static void
test_the_pose_does_not_move_before_hold_ms_and_does_after(void) {
    const gfx_glow_pose_t boot_pose = normalized_pose(11585, 11585);
    const gfx_glow_pose_t level = (gfx_glow_pose_t){0, GFX_GLOW_POSE_ONE};
    ridge_pose_t rp = {.pose = boot_pose, .level = level, .steady_level = level, .steady_ms = STEADY_HOLD_MS};

    for (uint32_t alive = 0; alive < HOLD_MS; alive += DT_MS) {
        ridge_pose_advance(&rp, DT_MS, alive, HOLD_MS, boot_pose, TAU_MS, STEADY_STEP, STEADY_HOLD_MS, REDRAW_STEP);
        TEST_ASSERT_EQUAL_INT32(boot_pose.down_x, rp.pose.down_x);
        TEST_ASSERT_EQUAL_INT32(boot_pose.down_y, rp.pose.down_y);
    }

    bool moved = false;
    for (uint32_t alive = HOLD_MS; alive < HOLD_MS + 2000 && !moved; alive += DT_MS) {
        ridge_pose_advance(&rp, DT_MS, alive, HOLD_MS, boot_pose, TAU_MS, STEADY_STEP, STEADY_HOLD_MS, REDRAW_STEP);
        moved = rp.pose.down_x != boot_pose.down_x || rp.pose.down_y != boot_pose.down_y;
    }
    TEST_ASSERT_TRUE_MESSAGE(moved, "gravity never took hold after the boot hold");
}

static void
test_gravity_below_threshold_leaves_the_level_where_it_was(void) {
    const gfx_glow_pose_t level = normalized_pose(-7000, 13000);
    const gfx_glow_pose_t left_alone = ridge_pose_level_from_gravity(level, 100, 50, 30, 64);
    TEST_ASSERT_EQUAL_INT32(level.down_x, left_alone.down_x);
    TEST_ASSERT_EQUAL_INT32(level.down_y, left_alone.down_y);

    const gfx_glow_pose_t changed = ridge_pose_level_from_gravity(level, 0, 4096, 256, 64);
    TEST_ASSERT_EQUAL_INT32(0, changed.down_x);
    TEST_ASSERT_TRUE(changed.down_y > 0);
}

static void
test_once_steady_the_pose_is_exactly_the_target_not_merely_close(void) {
    const gfx_glow_pose_t target = normalized_pose(5000, 15000);
    ridge_pose_t rp = {
        .pose = (gfx_glow_pose_t){target.down_x + REDRAW_STEP / 2, target.down_y},
        .level = target,
        .steady_level = target,
        .steady_ms = STEADY_HOLD_MS,
    };
    TEST_ASSERT_TRUE(rp.pose.down_x != target.down_x);

    const bool arrived =
        ridge_pose_advance(&rp, DT_MS, HOLD_MS + 1, HOLD_MS, target, TAU_MS, STEADY_STEP, STEADY_HOLD_MS, REDRAW_STEP);
    TEST_ASSERT_TRUE(arrived);
    TEST_ASSERT_EQUAL_INT32(target.down_x, rp.pose.down_x);
    TEST_ASSERT_EQUAL_INT32(target.down_y, rp.pose.down_y);
}

static void
test_a_jittering_target_never_triggers_the_snap(void) {
    const gfx_glow_pose_t a = (gfx_glow_pose_t){GFX_GLOW_POSE_ONE, 0};
    const gfx_glow_pose_t b = (gfx_glow_pose_t){0, GFX_GLOW_POSE_ONE};
    ridge_pose_t rp = {.pose = a, .level = a, .steady_level = a, .steady_ms = 0};
    bool ever_arrived = false;
    for (int frame = 0; frame < 500; frame++) {
        rp.level = (frame % 2 == 0) ? a : b;
        const bool arrived =
            ridge_pose_advance(&rp, DT_MS, HOLD_MS + 1, HOLD_MS, a, TAU_MS, STEADY_STEP, STEADY_HOLD_MS, REDRAW_STEP);
        ever_arrived = ever_arrived || arrived;
    }
    TEST_ASSERT_FALSE_MESSAGE(ever_arrived, "a jittering target snapped anyway");
}

static void
test_at_rest_with_a_steady_target_nothing_drifts(void) {
    const gfx_glow_pose_t target = normalized_pose(-9000, 5000);
    ridge_pose_t rp = {.pose = target, .level = target, .steady_level = target, .steady_ms = STEADY_HOLD_MS};
    for (int frame = 0; frame < 5000; frame++) {
        ridge_pose_advance(&rp, DT_MS, HOLD_MS + 1, HOLD_MS, target, TAU_MS, STEADY_STEP, STEADY_HOLD_MS, REDRAW_STEP);
        TEST_ASSERT_EQUAL_INT32(target.down_x, rp.pose.down_x);
        TEST_ASSERT_EQUAL_INT32(target.down_y, rp.pose.down_y);
    }
}

static void
test_slope_is_zero_when_aligned_and_signed_by_the_lag(void) {
    const gfx_glow_pose_t level = normalized_pose(0, 16384);
    ridge_pose_t rp = {.pose = level, .level = level};
    TEST_ASSERT_EQUAL_INT32(0, ridge_pose_slope(&rp));

    rp.pose = normalized_pose(5000, 15000);
    const int32_t lagged_one_way = ridge_pose_slope(&rp);
    rp.pose = normalized_pose(-5000, 15000);
    const int32_t lagged_other_way = ridge_pose_slope(&rp);
    TEST_ASSERT_TRUE(lagged_one_way != 0 && lagged_other_way != 0);
    TEST_ASSERT_TRUE((lagged_one_way > 0) != (lagged_other_way > 0));
}

#define COLUMN_TEST_COLUMNS 301
#define COLUMN_TEST_PANEL_W 97
#define COLUMN_TEST_PANEL_H 81

static void
test_column_under_maps_the_panels_centre_to_the_curves_centre_at_axis_poses(void) {
    const int centre_x = (COLUMN_TEST_PANEL_W - 1) / 2;
    const int centre_y = (COLUMN_TEST_PANEL_H - 1) / 2;
    const int expected = (COLUMN_TEST_COLUMNS - 1) / 2;
    const gfx_glow_pose_t axis[] = {
        {GFX_GLOW_POSE_ONE, 0},
        {-GFX_GLOW_POSE_ONE, 0},
        {0, GFX_GLOW_POSE_ONE},
        {0, -GFX_GLOW_POSE_ONE},
    };
    for (size_t i = 0; i < sizeof axis / sizeof axis[0]; i++) {
        const int column = ridge_pose_column_under(axis[i], COLUMN_TEST_PANEL_W, COLUMN_TEST_PANEL_H,
                                                   COLUMN_TEST_COLUMNS, centre_x, centre_y);
        TEST_ASSERT_EQUAL_INT(expected, column);
    }

    /* Off centre too, along whichever panel axis a pose reads as "along the
     * line": down (-1, 0) reads panel y, down (0, 1) reads panel x. */
    const int landscape_column =
        ridge_pose_column_under((gfx_glow_pose_t){-GFX_GLOW_POSE_ONE, 0}, COLUMN_TEST_PANEL_W, COLUMN_TEST_PANEL_H,
                                COLUMN_TEST_COLUMNS, centre_x, centre_y + 10);
    TEST_ASSERT_EQUAL_INT(expected + 10, landscape_column);
    const int portrait_column =
        ridge_pose_column_under((gfx_glow_pose_t){0, GFX_GLOW_POSE_ONE}, COLUMN_TEST_PANEL_W, COLUMN_TEST_PANEL_H,
                                COLUMN_TEST_COLUMNS, centre_x + 10, centre_y);
    TEST_ASSERT_EQUAL_INT(expected + 10, portrait_column);
}

void
suite_ridge_pose(void) {
    RUN_TEST(test_an_eased_pose_stays_a_unit_vector_over_a_long_turn);
    RUN_TEST(test_a_quarter_turn_arrives_and_stops);
    RUN_TEST(test_a_half_turn_between_exactly_opposed_poses_turns_and_arrives);
    RUN_TEST(test_the_pose_does_not_move_before_hold_ms_and_does_after);
    RUN_TEST(test_gravity_below_threshold_leaves_the_level_where_it_was);
    RUN_TEST(test_once_steady_the_pose_is_exactly_the_target_not_merely_close);
    RUN_TEST(test_a_jittering_target_never_triggers_the_snap);
    RUN_TEST(test_at_rest_with_a_steady_target_nothing_drifts);
    RUN_TEST(test_slope_is_zero_when_aligned_and_signed_by_the_lag);
    RUN_TEST(test_column_under_maps_the_panels_centre_to_the_curves_centre_at_axis_poses);
}

SUITE_REGISTER(suite_ridge_pose);
