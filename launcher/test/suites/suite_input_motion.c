/* Shared motion readers: defaults, sample caching and bounded time. */
#include "suites.h"
#ifndef DEVICE_BUILD
#include <math.h>
#include "../input_examples/gravity_shared.c"
#include "clock_fake.h"
#include "input/motion/gravity.h"
#include "input/motion/motion_reader.h"
#include "input/motion/shake.h"
#include "input_fake.h"
#include "testkit/cleanup.h"
#include "unity.h"
#define TEST_APP_STEPS   8
#define TEST_TURN_DPS    90
#define TEST_EPSILON     0.00001F
#define TEST_STEP_US     (16U * MATH_US_PER_MS)
#define TEST_LONG_GAP_US (GRAVITY_MAX_DT_US * 20U)

static void
fixture(void) {
    suite_add_test_cleanup(clock_fake_reset);
    motion_reader_reset();
    input_fake_motion(NULL);
    clock_fake_set(0);
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

void
run_input_motion_suite(void) {
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
