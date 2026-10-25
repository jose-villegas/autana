/* Shared motion readers: defaults, lazy frames and bounded time. */
#include "suites.h"
#ifndef DEVICE_BUILD
#include <math.h>
#include "../input_examples/gravity_shared.c"
#include "input/motion/gravity.h"
#include "input/motion/input_motion.h"
#include "input/motion/shake.h"
#include "input_fake.h"
#include "unity.h"
#define TEST_EPSILON     0.00001F
#define TEST_STEP_US     (16U * MATH_US_PER_MS)
#define TEST_LONG_GAP_US (GRAVITY_MAX_DT_US * 20U)

static void
fixture(void) {
    input_motion_reset();
    input_fake_motion(NULL);
    input_motion_frame(0);
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
test_first_reader_updates_once_per_frame(void) {
    for (int shake_first = 0; shake_first < 2; shake_first++) {
        fixture();
        input_motion_t motion = {.acceleration = {0, INPUT_ACCEL_ONE_G, 0}};
        input_fake_motion(&motion);
        input_motion_frame(TEST_STEP_US);
        motion.acceleration = (vec3i_t){INPUT_ACCEL_ONE_G, 0, 0};
        if (shake_first) {
            TEST_ASSERT_EQUAL_FLOAT(0, input_shake());
        }
        gravity_shared_example_t example = {0};
        gravity_shared_example(&example, TEST_STEP_US / MATH_US_PER_MS);
        TEST_ASSERT_EQUAL_FLOAT(0, example.shake);
        TEST_ASSERT_FLOAT_WITHIN(TEST_EPSILON, 1, example.strength);
        TEST_ASSERT_FLOAT_WITHIN(TEST_EPSILON, 1, input_gravity().x);
        motion.acceleration = (vec3i_t){0};
        TEST_ASSERT_FLOAT_WITHIN(TEST_EPSILON, 1, input_gravity().x);
        input_motion_frame(TEST_STEP_US * 2U);
        TEST_ASSERT_EQUAL_FLOAT(0, input_gravity().x);
        TEST_ASSERT_EQUAL_FLOAT(0, input_gravity().y);
        TEST_ASSERT_EQUAL_FLOAT(0, input_gravity().z);
        input_fake_motion(NULL);
    }
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
    input_motion_frame(TEST_LONG_GAP_US);
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
    RUN_TEST(test_first_reader_updates_once_per_frame);
    RUN_TEST(test_dt_clamp_and_strength);
}

SUITE_REGISTER(run_input_motion_suite);
#endif
