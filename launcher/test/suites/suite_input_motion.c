/* Shared motion readers: defaults, sample caching and bounded time. */
#include "suites.h"
#ifndef DEVICE_BUILD
#include <math.h>
#include "../input_examples/gravity_shared.c"
#include "clock_fake.h"
#include "input/input_shell.h"
#include "input/motion/gravity.h"
#include "input/motion/imu_calib.h"
#include "input/motion/imu_rest.h"
#include "input/motion/motion_reader.h"
#include "input/motion/shake.h"
#include "input_fake.h"
#include "math/scalar/math_const.h"
#include "testkit/cleanup.h"
#include "unity.h"
#define TEST_APP_STEPS   8
#define TEST_TURN_DPS    90
#define TEST_EPSILON     0.00001F
#define TEST_STEP_US     (16U * MATH_US_PER_MS)
#define TEST_LONG_GAP_US (GRAVITY_MAX_DT_US * 20U)

static void
fixture(void) {
    suite_set_test_cleanup(input_fake_reset);
    suite_add_test_cleanup(clock_fake_reset);
    motion_reader_reset();
    input_fake_motion(NULL);
    clock_fake_set(0);
}

static void
test_fake_owns_sample_after_scope(void) {
    fixture();
    input_motion_t expected = {.acceleration = {INPUT_ACCEL_ONE_G, 0, 0}, .rotation = {0, 0, INPUT_ROTATION_ONE_DPS}};
    expected.turn_speed = imu_turn_speed(expected.rotation);
    input_motion_t actual;
    uint32_t sequence;
    {
        input_motion_t motion = expected;
        input_fake_motion(&motion);
        TEST_ASSERT_TRUE(input_read_motion_sequence(&actual, &sequence));
        motion = (input_motion_t){0};
        TEST_ASSERT_TRUE(input_read_motion(&actual));
        TEST_ASSERT_EQUAL_MEMORY(&expected, &actual, sizeof expected);
    }
    uint32_t held_sequence;
    TEST_ASSERT_TRUE(input_read_motion_sequence(&actual, &held_sequence));
    TEST_ASSERT_EQUAL_MEMORY(&expected, &actual, sizeof expected);
    TEST_ASSERT_EQUAL_UINT32(sequence, held_sequence);
    input_fake_motion(&expected);
    TEST_ASSERT_TRUE(input_read_motion_sequence(&actual, &held_sequence));
    TEST_ASSERT_EQUAL_UINT32(sequence + 1, held_sequence);
    input_fake_motion(NULL);
    TEST_ASSERT_FALSE(input_read_motion(&actual));
}

static void
test_no_sensor_defaults(void) {
    fixture();
    const vec3f_t g = input_gravity();
    TEST_ASSERT_EQUAL_FLOAT(0, g.x);
    TEST_ASSERT_EQUAL_FLOAT(1, g.y);
    TEST_ASSERT_EQUAL_FLOAT(0, g.z);
    TEST_ASSERT_EQUAL_FLOAT(0, input_shake());
}

static void
test_present_without_sample_is_zero(void) {
    fixture();
    input_fake_motion_present(true);
    for (int i = 0; i < 3; i++) {
        clock_fake_set(i * TEST_STEP_US);
        const vec3f_t g = input_gravity();
        TEST_ASSERT_EQUAL_FLOAT(0, g.x);
        TEST_ASSERT_EQUAL_FLOAT(0, g.y);
        TEST_ASSERT_EQUAL_FLOAT(0, g.z);
        TEST_ASSERT_EQUAL_FLOAT(0, input_shake());
    }
    input_fake_motion(NULL);
}

static void
test_missed_read_holds_estimate_and_elapsed_time(void) {
    const uint32_t gaps[] = {TEST_STEP_US * 2, TEST_LONG_GAP_US};
    for (size_t i = 0; i < sizeof gaps / sizeof gaps[0]; i++) {
        fixture();
        input_motion_t motion = {.acceleration = {0, INPUT_ACCEL_ONE_G, 0}};
        input_fake_motion(&motion);
        gravity_t expected;
        gravity_reset(&expected);
        shake_t expected_shake = {0};
        gravity_update(&expected, &motion, 0);
        (void)input_gravity();
        motion.acceleration.x = INPUT_ACCEL_ONE_G * 4;
        motion.rotation.z = INPUT_ROTATION_ONE_DPS * TEST_TURN_DPS;
        input_fake_motion(&motion);
        clock_fake_set(GRAVITY_MAX_DT_US);
        gravity_update(&expected, &motion, GRAVITY_MAX_DT_US);
        shake_update(&expected_shake, gravity_linear(&expected), GRAVITY_MAX_DT_US);
        const vec3f_t previous = input_gravity();
        const float previous_shake = input_shake();
        TEST_ASSERT_GREATER_THAN_FLOAT(0, previous_shake);
        input_fake_motion(NULL);
        input_fake_motion_present(true);
        clock_fake_set(GRAVITY_MAX_DT_US + gaps[i] / 2);
        const vec3f_t held = input_gravity();
        TEST_ASSERT_EQUAL_MEMORY(&previous, &held, sizeof held);
        TEST_ASSERT_EQUAL_FLOAT(previous_shake, input_shake());
        input_fake_motion(&motion);
        clock_fake_set(GRAVITY_MAX_DT_US + gaps[i]);
        const uint32_t dt = gaps[i] > GRAVITY_MAX_DT_US ? GRAVITY_MAX_DT_US : gaps[i];
        gravity_update(&expected, &motion, dt);
        shake_update(&expected_shake, gravity_linear(&expected), dt);
        const vec3f_t g = input_gravity();
        const vec3f_t reference = gravity_down(&expected);
        TEST_ASSERT_FLOAT_WITHIN(TEST_EPSILON, reference.x, g.x);
        TEST_ASSERT_FLOAT_WITHIN(TEST_EPSILON, reference.y, g.y);
        TEST_ASSERT_FLOAT_WITHIN(TEST_EPSILON, reference.z, g.z);
        TEST_ASSERT_FLOAT_WITHIN(TEST_EPSILON, shake_level(&expected_shake), input_shake());
        input_fake_motion(NULL);
    }
}

static void
test_cached_read(void) {
    fixture();
    input_motion_t motion = {.acceleration = {INPUT_ACCEL_ONE_G, 0, 0}};
    input_fake_motion(&motion);
    const vec3f_t first = input_gravity();
    const float shake = input_shake();
    motion.acceleration = (vec3i_t){0};
    clock_fake_set(TEST_LONG_GAP_US);
    const vec3f_t cached = input_gravity();
    TEST_ASSERT_EQUAL_MEMORY(&first, &cached, sizeof first);
    TEST_ASSERT_EQUAL_FLOAT(shake, input_shake());
    input_fake_motion(NULL);
}

static void
test_reader_order_independence(void) {
    vec3f_t down[2];
    float shake[2];
    for (int order = 0; order < 2; order++) {
        fixture();
        input_motion_t motion = {.acceleration = {0, INPUT_ACCEL_ONE_G, 0}};
        input_fake_motion(&motion);
        (void)input_gravity();
        motion.acceleration.x = (int)(SHAKE_FULL_G * 4 * INPUT_ACCEL_ONE_G);
        input_fake_motion(&motion);
        clock_fake_set(TEST_STEP_US);
        if (order) {
            shake[order] = input_shake();
            down[order] = input_gravity();
        } else {
            down[order] = input_gravity();
            shake[order] = input_shake();
        }
        input_fake_motion(NULL);
    }
    TEST_ASSERT_EQUAL_MEMORY(&down[0], &down[1], sizeof down[0]);
    TEST_ASSERT_GREATER_THAN_FLOAT(0, shake[0]);
    TEST_ASSERT_EQUAL_FLOAT(shake[0], shake[1]);
}

static void
test_new_sample_without_input_poll(void) {
    fixture();
    input_motion_t motion = {.acceleration = {0, INPUT_ACCEL_ONE_G, 0},
                             .rotation = {0, 0, INPUT_ROTATION_ONE_DPS * TEST_TURN_DPS}};
    gravity_t expected;
    gravity_reset(&expected);
    shake_t expected_shake = {0};
    gravity_shared_example_t app = {0};
    for (unsigned step = 0; step < TEST_APP_STEPS; step++) {
        clock_fake_set((int64_t)step * TEST_STEP_US);
        input_fake_motion(&motion);
        const uint32_t dt = step ? TEST_STEP_US : 0;
        gravity_update(&expected, &motion, dt);
        shake_update(&expected_shake, gravity_linear(&expected), dt);
        gravity_shared_example(&app, TEST_STEP_US / MATH_US_PER_MS);
        const vec3f_t reference = gravity_down(&expected);
        const vec3f_t actual = input_gravity();
        TEST_ASSERT_FLOAT_WITHIN(TEST_EPSILON, reference.x, actual.x);
        TEST_ASSERT_FLOAT_WITHIN(TEST_EPSILON, reference.y, actual.y);
        TEST_ASSERT_FLOAT_WITHIN(TEST_EPSILON, shake_level(&expected_shake), app.shake);
    }
    TEST_ASSERT_GREATER_THAN_FLOAT(0, app.ball_x);
    input_fake_motion(NULL);
}

static void
test_free_fall_and_held_recovery(void) {
    fixture();
    input_motion_t motion = {0};
    input_fake_motion(&motion);
    const vec3f_t zero = {0};
    vec3f_t down = input_gravity();
    TEST_ASSERT_EQUAL_MEMORY(&zero, &down, sizeof zero);
    motion.acceleration.y = INPUT_ACCEL_ONE_G;
    input_fake_motion(&motion);
    clock_fake_set(TEST_STEP_US);
    down = input_gravity();
    TEST_ASSERT_FLOAT_WITHIN(TEST_EPSILON, 1, sqrtf(vec3f_dot(down, down)));
    input_fake_motion(NULL);
}

static void
test_sensor_loss_clears_shake(void) {
    fixture();
    input_motion_t motion = {.acceleration = {0, INPUT_ACCEL_ONE_G, 0}};
    input_fake_motion(&motion);
    (void)input_gravity();
    motion.acceleration.x = INPUT_ACCEL_ONE_G * 4;
    input_fake_motion(&motion);
    clock_fake_set(GRAVITY_MAX_DT_US);
    TEST_ASSERT_EQUAL_FLOAT(1, input_shake());
    input_fake_motion(NULL);
    clock_fake_set(GRAVITY_MAX_DT_US + TEST_STEP_US);
    TEST_ASSERT_EQUAL_FLOAT(0, input_shake());
    const vec3f_t fallback = {0, 1, 0};
    vec3f_t down = input_gravity();
    TEST_ASSERT_EQUAL_MEMORY(&fallback, &down, sizeof fallback);
    motion.acceleration.x = 0;
    input_fake_motion(&motion);
    clock_fake_set(GRAVITY_MAX_DT_US + TEST_STEP_US * 2);
    TEST_ASSERT_EQUAL_FLOAT(0, input_shake());
    input_fake_motion(NULL);
}

static void
test_first_and_nonmonotonic_times_have_zero_dt(void) {
    fixture();
    input_motion_t motion = {.acceleration = {0, INPUT_ACCEL_ONE_G, 0}};
    input_fake_motion(&motion);
    gravity_t expected;
    gravity_reset(&expected);
    shake_t expected_shake = {0};
    const int64_t times[] = {TEST_LONG_GAP_US, TEST_LONG_GAP_US + TEST_STEP_US, TEST_LONG_GAP_US + TEST_STEP_US,
                             TEST_LONG_GAP_US};
    for (size_t i = 0; i < sizeof times / sizeof times[0]; i++) {
        motion.acceleration.x = (int)(SHAKE_FULL_G * (float)(i + 2) * INPUT_ACCEL_ONE_G);
        motion.rotation.z = INPUT_ROTATION_ONE_DPS * TEST_TURN_DPS;
        const uint32_t dt = i == 1 ? TEST_STEP_US : 0;
        gravity_update(&expected, &motion, dt);
        shake_update(&expected_shake, gravity_linear(&expected), dt);
        input_fake_motion(&motion);
        clock_fake_set(times[i]);
        const vec3f_t down = input_gravity();
        const vec3f_t reference = gravity_down(&expected);
        TEST_ASSERT_FLOAT_WITHIN(TEST_EPSILON, reference.x, down.x);
        TEST_ASSERT_FLOAT_WITHIN(TEST_EPSILON, reference.y, down.y);
        TEST_ASSERT_FLOAT_WITHIN(TEST_EPSILON, reference.z, down.z);
        TEST_ASSERT_FLOAT_WITHIN(TEST_EPSILON, shake_level(&expected_shake), input_shake());
    }
    input_fake_motion(NULL);
}

static void
test_dt_clamp_and_strength(void) {
    fixture();
    input_motion_t motion = {.acceleration = {0, INPUT_ACCEL_ONE_G, 0}};
    input_fake_motion(&motion);
    (void)input_gravity();
    gravity_t expected;
    gravity_reset(&expected);
    gravity_update(&expected, &motion, 0);
    motion.acceleration.x = INPUT_ACCEL_ONE_G;
    motion.rotation.z = INPUT_ROTATION_ONE_DPS;
    gravity_update(&expected, &motion, GRAVITY_MAX_DT_US);
    shake_t shake = {0};
    shake_update(&shake, gravity_linear(&expected), GRAVITY_MAX_DT_US);
    input_fake_motion(&motion);
    clock_fake_set(TEST_LONG_GAP_US);
    const vec3f_t g = input_gravity();
    const vec3f_t down = gravity_down(&expected);
    TEST_ASSERT_FLOAT_WITHIN(TEST_EPSILON, down.x, g.x);
    TEST_ASSERT_FLOAT_WITHIN(TEST_EPSILON, down.y, g.y);
    TEST_ASSERT_FLOAT_WITHIN(TEST_EPSILON, gravity_strength(&expected), hypotf(g.x, g.y));
    TEST_ASSERT_FLOAT_WITHIN(TEST_EPSILON, shake_level(&shake), input_shake());
    input_fake_motion(NULL);
}

#define TEST_OFFSET_DPS         4
#define TEST_HALF_TURN_DEG      180.0F
#define TEST_DEGREES_PER_RAD    (TEST_HALF_TURN_DEG / MATH_PI)
#define TEST_FLAT_TOLERANCE_DEG 0.2F
#define TEST_LEARN_STEPS        (((IMU_REST_SETTLE_MS + IMU_REST_WINDOW_MS) * MATH_US_PER_MS) / TEST_STEP_US + 4)

static void
test_rest_learning_applies_and_clears_shared_bias(void) {
    fixture();
    input_motion_t raw = {.acceleration = {0, 0, -INPUT_ACCEL_ONE_G},
                          .rotation = {TEST_OFFSET_DPS * INPUT_ROTATION_ONE_DPS, 0, 0}};
    gravity_t reference;
    gravity_reset(&reference);
    bool learned = false;
    for (unsigned step = 0; step < TEST_LEARN_STEPS; step++) {
        clock_fake_set((int64_t)step * TEST_STEP_US);
        const vec3i_t previous = input_gyro_offset();
        input_motion_t corrected = raw;
        corrected.rotation = vec3i_sub(raw.rotation, previous);
        gravity_update(&reference, &corrected, step ? TEST_STEP_US : 0);
        input_fake_motion(&raw);
        const vec3f_t actual = input_gravity();
        const vec3i_t offset = input_gyro_offset();
        if (!vec3i_equal(previous, offset)) {
            learned = true;
            gravity_clear_gyro_bias(&reference);
        }
        const vec3f_t expected = gravity_down(&reference);
        TEST_ASSERT_FLOAT_WITHIN(TEST_EPSILON, expected.x, actual.x);
        TEST_ASSERT_FLOAT_WITHIN(TEST_EPSILON, expected.y, actual.y);
        TEST_ASSERT_FLOAT_WITHIN(TEST_EPSILON, expected.z, actual.z);
    }
    TEST_ASSERT_TRUE(learned);
    TEST_ASSERT_EQUAL_INT(raw.rotation.x, input_gyro_offset().x);
    input_motion_t actual;
    TEST_ASSERT_TRUE(input_read_motion(&actual));
    TEST_ASSERT_EQUAL_INT(0, actual.rotation.x);
    TEST_ASSERT_EQUAL_INT(0, actual.turn_speed);
}

static void
test_corrected_flat_is_accurate_from_boot(void) {
    fixture();
    const input_motion_t raw = {.acceleration = {0, 0, -INPUT_ACCEL_ONE_G},
                                .rotation = {TEST_OFFSET_DPS * INPUT_ROTATION_ONE_DPS, 0, 0}};
    input_fake_stored_gyro(raw.rotation);
    gravity_t uncorrected;
    gravity_reset(&uncorrected);
    for (uint32_t t = 0; t <= MATH_US_PER_S; t += TEST_STEP_US) {
        clock_fake_set(t);
        input_fake_motion(&raw);
        const vec3f_t down = input_gravity();
        const float degrees = acosf(fmaxf(-1.0F, fminf(1.0F, -down.z))) * TEST_DEGREES_PER_RAD;
        TEST_ASSERT_LESS_THAN_FLOAT(TEST_FLAT_TOLERANCE_DEG, degrees);
        gravity_update(&uncorrected, &raw, t ? TEST_STEP_US : 0);
    }
    const vec3f_t down = gravity_down(&uncorrected);
    TEST_ASSERT_GREATER_THAN_FLOAT(TEST_FLAT_TOLERANCE_DEG, acosf(-down.z) * TEST_DEGREES_PER_RAD);
}

static void
test_second_window_does_not_double_apply(void) {
    fixture();
    const input_motion_t raw = {.acceleration = {0, 0, -INPUT_ACCEL_ONE_G},
                                .rotation = {TEST_OFFSET_DPS * INPUT_ROTATION_ONE_DPS, 0, 0}};
    for (unsigned step = 0; step < TEST_LEARN_STEPS * 3; step++) {
        clock_fake_set((int64_t)step * TEST_STEP_US);
        input_fake_motion(&raw);
        input_gravity();
        if (step >= TEST_LEARN_STEPS) {
            TEST_ASSERT_TRUE(vec3i_equal(raw.rotation, input_gyro_offset()));
        }
    }
}

static void
test_learning_window_interruptions(void) {
    for (int interruption = 0; interruption < 3; interruption++) {
        fixture();
        const input_motion_t raw = {.acceleration = {0, 0, -INPUT_ACCEL_ONE_G},
                                    .rotation = {TEST_OFFSET_DPS * INPUT_ROTATION_ONE_DPS, 0, 0}};
        int64_t now = 0;
        for (unsigned step = 0; step < TEST_LEARN_STEPS / 2; step++) {
            clock_fake_set(now);
            now += TEST_STEP_US;
            input_fake_motion(&raw);
            input_gravity();
        }
        if (interruption == 0) {
            input_fake_motion(NULL);
            input_gravity();
        }
        if (interruption == 1) {
            motion_reader_reset();
        }
        if (interruption == 2) {
            now += IMU_REST_MAX_GAP_MS * MATH_US_PER_MS + 1;
        }
        for (unsigned step = 0; step < TEST_LEARN_STEPS / 2; step++) {
            clock_fake_set(now);
            now += TEST_STEP_US;
            input_fake_motion(&raw);
            input_gravity();
            TEST_ASSERT_TRUE(vec3i_equal((vec3i_t){0}, input_gyro_offset()));
        }
        input_fake_reset();
    }
}

static void
test_rejected_learning_preserves_filter_bias(void) {
    fixture();
    const input_motion_t raw = {.acceleration = {0, 0, -INPUT_ACCEL_ONE_G}, .rotation = {IMU_CALIB_GYRO_MAX + 1, 0, 0}};
    gravity_t reference;
    gravity_reset(&reference);
    for (unsigned step = 0; step < TEST_LEARN_STEPS * 2; step++) {
        clock_fake_set((int64_t)step * TEST_STEP_US);
        input_fake_motion(&raw);
        gravity_update(&reference, &raw, step ? TEST_STEP_US : 0);
        const vec3f_t actual = input_gravity();
        const vec3f_t expected = gravity_down(&reference);
        TEST_ASSERT_FLOAT_WITHIN(TEST_EPSILON, expected.y, actual.y);
        TEST_ASSERT_FLOAT_WITHIN(TEST_EPSILON, expected.z, actual.z);
    }
    TEST_ASSERT_TRUE(vec3i_equal((vec3i_t){0}, input_gyro_offset()));
    TEST_ASSERT_GREATER_THAN_FLOAT(0, fabsf(reference.gyro_bias.x));
}

static void
test_negative_rest_mean_excludes_settling(void) {
    fixture();
    const uint32_t period_us = 10U * MATH_US_PER_MS;
    const uint32_t settle_steps = IMU_REST_SETTLE_MS * MATH_US_PER_MS / period_us;
    const uint32_t window_steps = IMU_REST_WINDOW_MS * MATH_US_PER_MS / period_us;
    const int32_t bias = -TEST_OFFSET_DPS * INPUT_ROTATION_ONE_DPS;
    for (uint32_t step = 0; step < settle_steps + window_steps + 2; step++) {
        input_motion_t raw = {
            .acceleration = {0, 0, -INPUT_ACCEL_ONE_G},
            .rotation = {step < settle_steps + 2 ? bias + INPUT_ROTATION_ONE_DPS : bias - (int32_t)(step % 2), 0, 0}};
        clock_fake_set((int64_t)step * period_us);
        input_fake_motion(&raw);
        input_gravity();
    }
    TEST_ASSERT_EQUAL_INT(bias - 1, input_gyro_offset().x);
}

static void
test_learning_magnitude_boundary(void) {
    const int32_t limit = (int32_t)(INPUT_ACCEL_ONE_G * (1.0F + GRAVITY_TRUST_FULL_G));
    for (int side = 0; side <= 1; side++) {
        fixture();
        const input_motion_t raw = {.acceleration = {0, 0, -limit - side},
                                    .rotation = {TEST_OFFSET_DPS * INPUT_ROTATION_ONE_DPS, 0, 0}};
        for (unsigned step = 0; step < TEST_LEARN_STEPS * 2; step++) {
            clock_fake_set((int64_t)step * TEST_STEP_US);
            input_fake_motion(&raw);
            input_gravity();
        }
        TEST_ASSERT_EQUAL_INT(side ? 0 : raw.rotation.x, input_gyro_offset().x);
        input_fake_reset();
    }
}

void
run_input_motion_suite(void) {
    RUN_TEST(test_negative_rest_mean_excludes_settling);
    RUN_TEST(test_second_window_does_not_double_apply);
    RUN_TEST(test_learning_window_interruptions);
    RUN_TEST(test_rejected_learning_preserves_filter_bias);
    RUN_TEST(test_learning_magnitude_boundary);
    RUN_TEST(test_rest_learning_applies_and_clears_shared_bias);
    RUN_TEST(test_corrected_flat_is_accurate_from_boot);
    RUN_TEST(test_fake_owns_sample_after_scope);
    RUN_TEST(test_no_sensor_defaults);
    RUN_TEST(test_present_without_sample_is_zero);
    RUN_TEST(test_missed_read_holds_estimate_and_elapsed_time);
    RUN_TEST(test_cached_read);
    RUN_TEST(test_reader_order_independence);
    RUN_TEST(test_new_sample_without_input_poll);
    RUN_TEST(test_free_fall_and_held_recovery);
    RUN_TEST(test_sensor_loss_clears_shake);
    RUN_TEST(test_first_and_nonmonotonic_times_have_zero_dt);
    RUN_TEST(test_dt_clamp_and_strength);
}

SUITE_REGISTER(run_input_motion_suite);
#endif
