#include <math.h>
#include <stddef.h>

#include "suites.h"
#include "unity.h"

#include "input/gravity.h"
#include "math/linear/math_const.h"
#include "math/linear/vec_convert.h"
#include "math/scalar/units.h"

#define STEP_MS              (GRAVITY_MAX_DT_MS / 10U)
#define SETTLE_MS            ((uint32_t)(GRAVITY_TAU_S * MATH_MS_PER_S) * 10U)
#define BIAS_SETTLE_MS       ((uint32_t)(GRAVITY_BIAS_TAU_S * MATH_MS_PER_S) * 10U)
#define QUARTER_TURN_DEG     90.0F
#define TURN_MS              (GRAVITY_MAX_DT_MS * 2U)
#define TURN_DPS             (QUARTER_TURN_DEG * MATH_MS_PER_S / TURN_MS)
#define FLOAT_TOLERANCE      (1.0F / INPUT_ACCEL_ONE_G)
#define ANGLE_TOLERANCE_DEG  (GRAVITY_REJECT_DEG / 10.0F)
#define TURN_TOLERANCE       (ANGLE_TOLERANCE_DEG * MATH_PI / (2.0F * QUARTER_TURN_DEG))
#define SHAKE_G              (GRAVITY_TRUST_NONE_G + GRAVITY_TRUST_FULL_G)
#define BIAS_DPS             (GRAVITY_STILL_DPS / 4.0F)
#define BIAS_TOLERANCE       (BIAS_DPS / 20.0F)
#define CORRECTION_ANGLE_RAD (GRAVITY_REJECT_DEG * MATH_PI / (4.0F * QUARTER_TURN_DEG))
#define PARTIAL_TRUST_G      ((GRAVITY_TRUST_FULL_G + GRAVITY_TRUST_NONE_G) / 2.0F)
#define STALLED_MS           (GRAVITY_MAX_DT_MS * 20U)

static gravity_t g;

static void
fixture(void) {
    gravity_reset(&g);
}

static input_motion_t
sample(vec3f_t a, vec3f_t w) {
    return (input_motion_t){.acceleration = vec3i_from_vec3f(vec3f_scale(a, INPUT_ACCEL_ONE_G)),
                            .rotation = vec3i_from_vec3f(vec3f_scale(w, INPUT_ROTATION_ONE_DPS))};
}

static void
hold(input_motion_t m, uint32_t duration_ms) {
    for (uint32_t elapsed = 0; elapsed < duration_ms; elapsed += STEP_MS) {
        gravity_update(&g, &m, STEP_MS);
    }
}

static void
assert_vector(vec3f_t expected, vec3f_t actual, float tolerance) {
    TEST_ASSERT_FLOAT_WITHIN(tolerance, expected.x, actual.x);
    TEST_ASSERT_FLOAT_WITHIN(tolerance, expected.y, actual.y);
    TEST_ASSERT_FLOAT_WITHIN(tolerance, expected.z, actual.z);
}

static void
test_stationary_poses_settle(void) {
    const vec3f_t poses[] = {{0, 0, -1}, {0, 1, 0}, {0, 0, 1}};
    for (size_t i = 0; i < sizeof poses / sizeof poses[0]; i++) {
        fixture();
        hold(sample(poses[i], (vec3f_t){0}), SETTLE_MS);
        assert_vector(poses[i], gravity_down(&g), FLOAT_TOLERANCE);
        TEST_ASSERT_FLOAT_WITHIN(FLOAT_TOLERANCE, fabsf(poses[i].y), gravity_strength(&g));
        TEST_ASSERT_FALSE(gravity_in_free_fall(&g));
    }
}

static void
test_first_reading_is_adopted_exactly(void) {
    fixture();
    const input_motion_t m = {.acceleration = {INPUT_ACCEL_ONE_G, INPUT_ACCEL_ONE_G, -INPUT_ACCEL_ONE_G}};
    gravity_update(&g, &m, 0);
    const vec3f_t expected = vec3f_normalize(vec3f_from_vec3i(m.acceleration));
    assert_vector(expected, gravity_down(&g), FLOAT_TOLERANCE);
    TEST_ASSERT_EQUAL_UINT32(0, g.distrust_ms);
}

static void
test_gyro_turns_follow_measured_screen_sense(void) {
    const struct {
        vec3f_t start, rate, end;
    } turns[] = {
        {{0, 1, 0}, {0, 0, TURN_DPS}, {1, 0, 0}},
        {{0, 1, 0}, {0, 0, -TURN_DPS}, {-1, 0, 0}},
        {{0, 0, -1}, {TURN_DPS, 0, 0}, {0, -1, 0}},
        {{0, 0, -1}, {-TURN_DPS, 0, 0}, {0, 1, 0}},
    };

    for (size_t i = 0; i < sizeof turns / sizeof turns[0]; i++) {
        fixture();
        hold(sample(turns[i].start, (vec3f_t){0}), STEP_MS);
        hold(sample(turns[i].start, turns[i].rate), TURN_MS);
        assert_vector(turns[i].end, gravity_down(&g), TURN_TOLERANCE);
        TEST_ASSERT_TRUE(g.distrust_ms > 0);
    }
}

static void
test_shake_preserves_down_and_reports_linear(void) {
    fixture();
    const vec3f_t down = {0, 0, -1};
    hold(sample(down, (vec3f_t){0}), STEP_MS);
    const float min_dot = cosf(ANGLE_TOLERANCE_DEG * MATH_PI / (2.0F * QUARTER_TURN_DEG));
    for (uint32_t elapsed = 0; elapsed < GRAVITY_RECOVER_MS; elapsed += STEP_MS) {
        const float shake = (elapsed / STEP_MS) % 2U ? SHAKE_G : -SHAKE_G;
        const input_motion_t m = sample((vec3f_t){shake, 0, -1}, (vec3f_t){0});
        gravity_update(&g, &m, STEP_MS);
        TEST_ASSERT_TRUE(vec3f_dot(down, gravity_down(&g)) >= min_dot);
        assert_vector((vec3f_t){shake, 0, 0}, gravity_linear(&g), FLOAT_TOLERANCE);
    }
}

static void
test_still_bias_converges_without_turning_down(void) {
    const vec3f_t offsets[] = {{BIAS_DPS, 0, 0}, {0, -BIAS_DPS, 0}, {0, 0, BIAS_DPS}};
    for (size_t i = 0; i < sizeof offsets / sizeof offsets[0]; i++) {
        fixture();
        const vec3f_t down = {0, 0, -1};
        hold(sample(down, (vec3f_t){0}), STEP_MS);
        hold(sample(down, offsets[i]), BIAS_SETTLE_MS);
        assert_vector(offsets[i], g.gyro_bias, BIAS_TOLERANCE);
        assert_vector(down, gravity_down(&g), TURN_TOLERANCE);
    }
}

static void
test_rejected_direction_recovers_only_past_limit(void) {
    fixture();
    const vec3f_t start = {0, 0, -1};
    const vec3f_t target = {0, 1, 0};
    hold(sample(start, (vec3f_t){0}), STEP_MS);
    const input_motion_t m = sample(target, (vec3f_t){0});
    hold(m, GRAVITY_RECOVER_MS);
    assert_vector(start, gravity_down(&g), FLOAT_TOLERANCE);
    TEST_ASSERT_EQUAL_UINT32(GRAVITY_RECOVER_MS, g.distrust_ms);
    gravity_update(&g, &m, STEP_MS);
    assert_vector(target, gravity_down(&g), FLOAT_TOLERANCE);
    TEST_ASSERT_EQUAL_UINT32(0, g.distrust_ms);
}

static void
test_free_fall_stays_finite(void) {
    const vec3f_t poses[] = {{0}, {0, 1, 0}};
    for (size_t i = 0; i < sizeof poses / sizeof poses[0]; i++) {
        fixture();
        hold(sample(poses[i], (vec3f_t){0}), STEP_MS);
        hold((input_motion_t){0}, GRAVITY_RECOVER_MS + STEP_MS);
        TEST_ASSERT_TRUE(gravity_in_free_fall(&g));
        TEST_ASSERT_TRUE(isfinite(g.down.x) && isfinite(g.down.y) && isfinite(g.down.z));
        TEST_ASSERT_TRUE(isfinite(g.linear.x) && isfinite(g.linear.y) && isfinite(g.linear.z));
        TEST_ASSERT_TRUE(isfinite(g.gyro_bias.x) && isfinite(g.gyro_bias.y) && isfinite(g.gyro_bias.z));
        TEST_ASSERT_EQUAL_FLOAT(0, gravity_strength(&g));
    }
}

static void
test_trusted_acceleration_pulls_gradually_with_magnitude_weight(void) {
    fixture();
    const vec3f_t down = {0, 0, -1};
    const vec3f_t target = {sinf(CORRECTION_ANGLE_RAD), 0, -cosf(CORRECTION_ANGLE_RAD)};
    hold(sample(down, (vec3f_t){0}), STEP_MS);
    const input_motion_t full = sample(target, (vec3f_t){0});
    gravity_update(&g, &full, STEP_MS);
    const float full_pull = gravity_down(&g).x;
    TEST_ASSERT_TRUE(full_pull > 0 && full_pull < target.x);
    TEST_ASSERT_EQUAL_UINT32(0, g.distrust_ms);
    fixture();
    hold(sample(down, (vec3f_t){0}), STEP_MS);
    const input_motion_t partial = sample(vec3f_scale(target, 1.0F + PARTIAL_TRUST_G), (vec3f_t){0});
    gravity_update(&g, &partial, STEP_MS);
    TEST_ASSERT_TRUE(gravity_down(&g).x > 0 && gravity_down(&g).x < full_pull);
    hold(full, SETTLE_MS);
    assert_vector(target, gravity_down(&g), FLOAT_TOLERANCE);
}

static void
test_stalled_dt_is_clamped(void) {
    fixture();
    hold(sample((vec3f_t){0, 0, -1}, (vec3f_t){0}), STEP_MS);
    gravity_t clamped = g;
    const input_motion_t m = sample((vec3f_t){0, 1, 0}, (vec3f_t){BIAS_DPS, 0, 0});
    gravity_update(&g, &m, STALLED_MS);
    gravity_update(&clamped, &m, GRAVITY_MAX_DT_MS);
    TEST_ASSERT_EQUAL_UINT32(GRAVITY_MAX_DT_MS, g.distrust_ms);
    TEST_ASSERT_TRUE(g.down.y < 0);
    assert_vector(clamped.down, g.down, 0);
    assert_vector(clamped.linear, g.linear, 0);
    assert_vector(clamped.gyro_bias, g.gyro_bias, 0);
    TEST_ASSERT_EQUAL_UINT32(clamped.distrust_ms, g.distrust_ms);
}

void
run_gravity_suite(void) {
    RUN_TEST(test_stationary_poses_settle);
    RUN_TEST(test_first_reading_is_adopted_exactly);
    RUN_TEST(test_gyro_turns_follow_measured_screen_sense);
    RUN_TEST(test_shake_preserves_down_and_reports_linear);
    RUN_TEST(test_still_bias_converges_without_turning_down);
    RUN_TEST(test_rejected_direction_recovers_only_past_limit);
    RUN_TEST(test_free_fall_stays_finite);
    RUN_TEST(test_trusted_acceleration_pulls_gradually_with_magnitude_weight);
    RUN_TEST(test_stalled_dt_is_clamped);
}

SUITE_REGISTER(run_gravity_suite);
