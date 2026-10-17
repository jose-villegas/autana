#include <float.h>
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
#define BIAS_SETTLE_MS       ((uint32_t)(GRAVITY_BIAS_SETTLE_S * MATH_MS_PER_S))
#define QUARTER_TURN_DEG     90.0F
#define TURN_MS              (GRAVITY_MAX_DT_MS * 2U)
#define TURN_DPS             (QUARTER_TURN_DEG * MATH_MS_PER_S / TURN_MS)
#define FLOAT_TOLERANCE      (1.0F / INPUT_ACCEL_ONE_G)
#define ANGLE_TOLERANCE_DEG  (GRAVITY_REJECT_DEG / 10.0F)
#define TURN_TOLERANCE       (ANGLE_TOLERANCE_DEG * MATH_PI / (2.0F * QUARTER_TURN_DEG))
#define SHAKE_G              (GRAVITY_TRUST_NONE_G + GRAVITY_TRUST_FULL_G)
#define BIAS_DPS             GRAVITY_GYRO_OFFSET_TOL_DPS
#define BIAS_TOLERANCE       (BIAS_DPS / 50.0F)
#define CORRECTION_ANGLE_RAD (GRAVITY_REJECT_DEG * MATH_PI / (4.0F * QUARTER_TURN_DEG))
#define PARTIAL_TRUST_G      ((GRAVITY_TRUST_FULL_G + GRAVITY_TRUST_NONE_G) / 2.0F)
#define CLAMP_OFFSET_DPS     (GRAVITY_BIAS_MAX_DPS + GRAVITY_GYRO_OFFSET_TOL_DPS / 4.0F)
#define STALLED_MS           (GRAVITY_MAX_DT_MS * 20U)

static gravity_t g;

static void
fixture(void) {
    gravity_reset(&g);
}

static void
test_rejection_cosine_matches_angle(void) {
    const float radians_per_degree = MATH_PI / (2.0F * QUARTER_TURN_DEG);
    TEST_ASSERT_FLOAT_WITHIN(FLT_EPSILON, cosf(GRAVITY_REJECT_DEG * radians_per_degree), GRAVITY_REJECT_COS);
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
test_perpendicular_bias_converges(void) {
    const vec3f_t offsets[] = {{BIAS_DPS, 0, 0}, {0, -BIAS_DPS, 0}, {BIAS_DPS, -BIAS_DPS, 0}};
    const vec3f_t down = {0, 0, -1};
    for (size_t i = 0; i < sizeof offsets / sizeof offsets[0]; i++) {
        fixture();
        hold(sample(down, (vec3f_t){0}), STEP_MS);
        const input_motion_t m = sample(down, offsets[i]);
        for (uint32_t elapsed = 0; elapsed < BIAS_SETTLE_MS; elapsed += STEP_MS) {
            gravity_update(&g, &m, STEP_MS);
            TEST_ASSERT_TRUE(vec3f_dot(down, g.down) >= GRAVITY_REJECT_COS);
        }
        assert_vector(offsets[i], g.gyro_bias, BIAS_TOLERANCE);
        const float min_dot = cosf(ANGLE_TOLERANCE_DEG * MATH_PI / (2.0F * QUARTER_TURN_DEG));
        TEST_ASSERT_TRUE(vec3f_dot(down, gravity_down(&g)) >= min_dot);
    }
}

static void
test_along_down_bias_is_unobservable(void) {
    fixture();
    const vec3f_t down = {0, 0, -1};
    hold(sample(down, (vec3f_t){0, 0, BIAS_DPS}), BIAS_SETTLE_MS);
    assert_vector((vec3f_t){0}, g.gyro_bias, 0);
    assert_vector(down, gravity_down(&g), FLOAT_TOLERANCE);
}

static void
test_rejection_and_recovery_do_not_learn_bias(void) {
    const vec3f_t near_down = {sinf(CORRECTION_ANGLE_RAD), 0, -cosf(CORRECTION_ANGLE_RAD)};
    const vec3f_t rejected[] = {vec3f_scale(near_down, 1.0F + SHAKE_G), {1, 0, 0}};
    for (size_t i = 0; i < sizeof rejected / sizeof rejected[0]; i++) {
        fixture();
        hold(sample((vec3f_t){0, 0, -1}, (vec3f_t){0}), STEP_MS);
        const input_motion_t m = sample(rejected[i], (vec3f_t){0});
        hold(m, GRAVITY_RECOVER_MS);
        assert_vector((vec3f_t){0}, g.gyro_bias, 0);
        gravity_update(&g, &m, STEP_MS);
        assert_vector((vec3f_t){0}, g.gyro_bias, 0);
        assert_vector(vec3f_normalize(rejected[i]), g.down, FLOAT_TOLERANCE);
    }
}

static void
test_bias_clamps_each_axis(void) {
    const struct {
        vec3f_t down, offset, expected;
    } cases[] = {
        {{0, 0, -1}, {CLAMP_OFFSET_DPS, 0, 0}, {GRAVITY_BIAS_MAX_DPS, 0, 0}},
        {{0, 0, -1}, {-CLAMP_OFFSET_DPS, 0, 0}, {-GRAVITY_BIAS_MAX_DPS, 0, 0}},
        {{0, 0, -1}, {0, CLAMP_OFFSET_DPS, 0}, {0, GRAVITY_BIAS_MAX_DPS, 0}},
        {{0, 0, -1}, {0, -CLAMP_OFFSET_DPS, 0}, {0, -GRAVITY_BIAS_MAX_DPS, 0}},
        {{0, 1, 0}, {0, 0, CLAMP_OFFSET_DPS}, {0, 0, GRAVITY_BIAS_MAX_DPS}},
        {{0, 1, 0}, {0, 0, -CLAMP_OFFSET_DPS}, {0, 0, -GRAVITY_BIAS_MAX_DPS}},
    };

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        fixture();
        hold(sample(cases[i].down, (vec3f_t){0}), STEP_MS);
        const input_motion_t m = sample(cases[i].down, cases[i].offset);
        for (uint32_t elapsed = 0; elapsed < BIAS_SETTLE_MS; elapsed += STEP_MS) {
            gravity_update(&g, &m, STEP_MS);
            TEST_ASSERT_TRUE(fabsf(g.gyro_bias.x) <= GRAVITY_BIAS_MAX_DPS);
            TEST_ASSERT_TRUE(fabsf(g.gyro_bias.y) <= GRAVITY_BIAS_MAX_DPS);
            TEST_ASSERT_TRUE(fabsf(g.gyro_bias.z) <= GRAVITY_BIAS_MAX_DPS);
        }
        assert_vector(cases[i].expected, g.gyro_bias, 0);
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
    const float full_bias = fabsf(g.gyro_bias.y);
    TEST_ASSERT_TRUE(full_pull > 0 && full_pull < target.x);
    TEST_ASSERT_EQUAL_UINT32(0, g.distrust_ms);
    fixture();
    hold(sample(down, (vec3f_t){0}), STEP_MS);
    const input_motion_t partial = sample(vec3f_scale(target, 1.0F + PARTIAL_TRUST_G), (vec3f_t){0});
    gravity_update(&g, &partial, STEP_MS);
    TEST_ASSERT_TRUE(gravity_down(&g).x > 0 && gravity_down(&g).x < full_pull);
    TEST_ASSERT_TRUE(fabsf(g.gyro_bias.y) > 0 && fabsf(g.gyro_bias.y) < full_bias);
    const vec3f_t bias = g.gyro_bias;
    const vec3f_t predicted = g.down;
    gravity_update(&g, &full, 0);
    assert_vector(bias, g.gyro_bias, 0);
    assert_vector(predicted, g.down, FLOAT_TOLERANCE);
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

#define TRUST_RATIO_TOLERANCE   (GRAVITY_TRUST_FULL_G / 100.0F)
/* Independent reference keeps a gain change from moving its own settling deadline. */
#define BIAS_REFERENCE_SETTLE_S 20.0F
#define BIAS_REFERENCE_KI       (4.0F / (GRAVITY_TAU_S * BIAS_REFERENCE_SETTLE_S))

static gravity_t
correction_at_magnitude(int counts) {
    gravity_t state = {.down = {sinf(CORRECTION_ANGLE_RAD), 0, -cosf(CORRECTION_ANGLE_RAD)}};
    const input_motion_t m = {.acceleration = {0, 0, -counts}};
    gravity_update(&state, &m, STEP_MS);
    return state;
}

static void
test_trust_ramp_ratios_and_symmetry(void) {
    const int full_counts = (int)(GRAVITY_TRUST_FULL_G * INPUT_ACCEL_ONE_G);
    const int none_counts = (int)(GRAVITY_TRUST_NONE_G * INPUT_ACCEL_ONE_G);
    const int departures[] = {full_counts + 1, (3 * full_counts + none_counts) / 4, (full_counts + none_counts) / 2,
                              (full_counts + 3 * none_counts) / 4, none_counts - 1};
    const gravity_t full = correction_at_magnitude(INPUT_ACCEL_ONE_G);
    const float initial_x = sinf(CORRECTION_ANGLE_RAD);
    const float full_pull = initial_x - full.down.x;
    for (size_t i = 0; i < sizeof departures / sizeof departures[0]; i++) {
        const float error = (float)departures[i] / INPUT_ACCEL_ONE_G;
        const float weight = (GRAVITY_TRUST_NONE_G - error) / (GRAVITY_TRUST_NONE_G - GRAVITY_TRUST_FULL_G);
        const gravity_t above = correction_at_magnitude(INPUT_ACCEL_ONE_G + departures[i]);
        const gravity_t below = correction_at_magnitude(INPUT_ACCEL_ONE_G - departures[i]);
        TEST_ASSERT_FLOAT_WITHIN(TRUST_RATIO_TOLERANCE, weight, above.gyro_bias.y / full.gyro_bias.y);
        TEST_ASSERT_FLOAT_WITHIN(TRUST_RATIO_TOLERANCE, weight, (initial_x - above.down.x) / full_pull);
        assert_vector(above.down, below.down, FLT_EPSILON);
        assert_vector(above.gyro_bias, below.gyro_bias, FLT_EPSILON);
    }
}

static void
test_acceptance_restarts_recovery(void) {
    fixture();
    const input_motion_t accepted = {.acceleration = {0, 0, -INPUT_ACCEL_ONE_G}};
    const input_motion_t rejected = {.acceleration = {0, INPUT_ACCEL_ONE_G, 0}};
    gravity_update(&g, &accepted, STEP_MS);
    const uint32_t interval = GRAVITY_RECOVER_MS * 3U / 4U;
    hold(rejected, interval);
    gravity_update(&g, &accepted, STEP_MS);
    hold(rejected, interval);
    assert_vector((vec3f_t){0, 0, -1}, gravity_down(&g), FLOAT_TOLERANCE);
}

static void
test_pull_time_constant(void) {
    const input_motion_t m = {.acceleration = {0, 0, -INPUT_ACCEL_ONE_G}};
    g = (gravity_t){.down = {sinf(CORRECTION_ANGLE_RAD), 0, -cosf(CORRECTION_ANGLE_RAD)}};
    const float dt = (float)STEP_MS / MATH_MS_PER_S;
    const unsigned steps = (unsigned)(GRAVITY_TAU_S / dt);
    const float expected = g.down.x * powf(1.0F - dt / (GRAVITY_TAU_S + dt), (float)steps);
    for (unsigned i = 0; i < steps; i++) {
        g.gyro_bias = (vec3f_t){0};
        gravity_update(&g, &m, STEP_MS);
    }
    TEST_ASSERT_FLOAT_WITHIN(expected * GRAVITY_TRUST_FULL_G / 10.0F, expected, g.down.x);
}

static void
test_strength_uses_both_plane_axes(void) {
    const float angle = GRAVITY_REJECT_DEG * MATH_PI / QUARTER_TURN_DEG;
    const float plane = sinf(angle) / sqrtf(2.0F);
    g = (gravity_t){.down = {plane, plane, -cosf(angle)}};
    TEST_ASSERT_FLOAT_WITHIN(FLT_EPSILON, sinf(angle), gravity_strength(&g));
}

static void
test_bias_slow_pole_and_no_overshoot(void) {
    fixture();
    const input_motion_t m = sample((vec3f_t){0, 0, -1}, (vec3f_t){BIAS_DPS, 0, 0});
    const input_motion_t rest = {.acceleration = {0, 0, -INPUT_ACCEL_ONE_G}};
    gravity_update(&g, &rest, STEP_MS);
    const float inverse_tau = 1.0F / GRAVITY_TAU_S;
    const float slow = (inverse_tau - sqrtf(inverse_tau * inverse_tau - 4.0F * BIAS_REFERENCE_KI)) / 2.0F;
    const uint32_t early_ms = (uint32_t)(2.0F * MATH_MS_PER_S / slow);
    const uint32_t final_ms = (uint32_t)(6.0F * MATH_MS_PER_S / slow);
    for (uint32_t ms = 0; ms < final_ms; ms += STEP_MS) {
        gravity_update(&g, &m, STEP_MS);
        TEST_ASSERT_TRUE(gravity_gyro_bias(&g).x <= BIAS_DPS + BIAS_TOLERANCE);
        if (ms + STEP_MS <= early_ms) {
            TEST_ASSERT_TRUE(gravity_gyro_bias(&g).x < BIAS_DPS - BIAS_TOLERANCE);
        }
    }
    TEST_ASSERT_FLOAT_WITHIN(BIAS_TOLERANCE, BIAS_DPS, gravity_gyro_bias(&g).x);
}

void
run_gravity_suite(void) {
    RUN_TEST(test_trust_ramp_ratios_and_symmetry);
    RUN_TEST(test_acceptance_restarts_recovery);
    RUN_TEST(test_pull_time_constant);
    RUN_TEST(test_strength_uses_both_plane_axes);
    RUN_TEST(test_bias_slow_pole_and_no_overshoot);
    RUN_TEST(test_rejection_cosine_matches_angle);
    RUN_TEST(test_stationary_poses_settle);
    RUN_TEST(test_first_reading_is_adopted_exactly);
    RUN_TEST(test_gyro_turns_follow_measured_screen_sense);
    RUN_TEST(test_shake_preserves_down_and_reports_linear);
    RUN_TEST(test_perpendicular_bias_converges);
    RUN_TEST(test_along_down_bias_is_unobservable);
    RUN_TEST(test_rejection_and_recovery_do_not_learn_bias);
    RUN_TEST(test_bias_clamps_each_axis);
    RUN_TEST(test_rejected_direction_recovers_only_past_limit);
    RUN_TEST(test_free_fall_stays_finite);
    RUN_TEST(test_trusted_acceleration_pulls_gradually_with_magnitude_weight);
    RUN_TEST(test_stalled_dt_is_clamped);
}

SUITE_REGISTER(run_gravity_suite);
