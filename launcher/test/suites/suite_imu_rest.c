#include <math.h>
#include "input/motion/gravity.h"
#include "input/motion/imu_rest.h"
#include "math/linear/vec_convert.h"
#include "math/scalar/math_const.h"
#include "suites.h"
#include "unity.h"

#define REST_TEST_DT_US       (10U * MATH_US_PER_MS)
#define REST_TEST_ACCEL_NOISE (INPUT_ACCEL_ONE_G / 128)
#define REST_TEST_BIAS        (4 * INPUT_ROTATION_ONE_DPS)
#define REST_TEST_DURATION_US ((IMU_REST_SETTLE_MS + IMU_REST_WINDOW_MS) * MATH_US_PER_MS)

static bool
rest_test_update(imu_rest_t* rest, const input_motion_t* sample, uint32_t dt, vec3i_t* mean) {
    const imu_rest_stage_t stage =
        imu_rest_step(rest, sample, dt,
                      sample != NULL
                          && gravity_accel_trusted(sqrtf(vec3f_dot(vec3f_from_vec3i(sample->acceleration),
                                                                   vec3f_from_vec3i(sample->acceleration)))
                                                   / INPUT_ACCEL_ONE_G));
    if (stage == IMU_REST_DONE) {
        *mean = imu_rest_mean(rest).rotation;
    }
    return stage == IMU_REST_DONE;
}

static input_motion_t
fixture(void) {
    return (input_motion_t){.acceleration = {0, 0, -INPUT_ACCEL_ONE_G}, .rotation = {REST_TEST_BIAS, 0, 0}};
}

static bool
run_window(imu_rest_t* rest, input_motion_t sample, uint32_t duration, vec3i_t* mean) {
    bool complete = false;
    for (uint32_t t = 0; t < duration; t += REST_TEST_DT_US) {
        complete |= rest_test_update(rest, &sample, REST_TEST_DT_US, mean);
    }
    return complete;
}

static void
test_rest_bias_and_noise(void) {
    for (int phase = 0; phase < 7; phase++) {
        imu_rest_t rest = {0};
        input_motion_t sample = fixture();
        vec3i_t mean = {0};
        bool complete = false;
        /* A balanced +/-1 dps sequence has sigma 1 dps without random tail failures. */
        for (uint32_t t = 0; t < REST_TEST_DURATION_US + 2 * REST_TEST_DT_US; t += REST_TEST_DT_US) {
            sample.rotation.x =
                REST_TEST_BIAS
                + (((t / REST_TEST_DT_US + phase) % 2) ? INPUT_ROTATION_ONE_DPS : -INPUT_ROTATION_ONE_DPS);
            sample.acceleration.y =
                ((t / REST_TEST_DT_US + phase) % 2) ? REST_TEST_ACCEL_NOISE : -REST_TEST_ACCEL_NOISE;
            complete |= rest_test_update(&rest, &sample, REST_TEST_DT_US, &mean);
        }
        TEST_ASSERT_TRUE(complete);
        TEST_ASSERT_INT_WITHIN(1, REST_TEST_BIAS, mean.x);
        TEST_ASSERT_EQUAL_INT(0, mean.y);
        TEST_ASSERT_EQUAL_INT(0, mean.z);
    }
}

static void
test_rest_restarts_and_magnitude(void) {
    for (int interruption = 0; interruption < 3; interruption++) {
        imu_rest_t rest = {0};
        input_motion_t sample = fixture();
        vec3i_t mean;
        TEST_ASSERT_FALSE(
            run_window(&rest, sample, REST_TEST_DURATION_US - IMU_REST_WINDOW_MS * MATH_US_PER_MS / 2, &mean));
        uint32_t dt = REST_TEST_DT_US;
        if (interruption == 0) {
            sample.rotation.y = IMU_REST_GYRO_DEPARTURE * 2;
        }
        if (interruption == 1) {
            dt = IMU_REST_MAX_GAP_MS * MATH_US_PER_MS + 1;
        }
        if (interruption == 2) {
            sample.acceleration.z *= 2;
        }
        TEST_ASSERT_FALSE(rest_test_update(&rest, &sample, dt, &mean));
        TEST_ASSERT_FALSE(run_window(&rest, fixture(), IMU_REST_WINDOW_MS * MATH_US_PER_MS, &mean));
        TEST_ASSERT_TRUE(run_window(&rest, fixture(), REST_TEST_DURATION_US, &mean));
    }
    imu_rest_t rest = {0};
    input_motion_t sample = fixture();
    sample.acceleration.z *= 2;
    vec3i_t mean;
    TEST_ASSERT_FALSE(run_window(&rest, sample, REST_TEST_DURATION_US * 2, &mean));
}

static void
test_rest_smooth_tilt(void) {
    for (int rate = 1; rate <= 5; rate += 4) {
        imu_rest_t rest = {0};
        input_motion_t sample = fixture();
        vec3i_t mean;
        bool complete = false;
        for (uint32_t t = 0; t < REST_TEST_DURATION_US * 3; t += REST_TEST_DT_US) {
            const float angle = (float)rate * (float)t / MATH_US_PER_S * MATH_RADIANS_PER_DEGREE;
            sample.acceleration.y = (int32_t)lroundf(sinf(angle) * INPUT_ACCEL_ONE_G);
            sample.acceleration.z = -(int32_t)lroundf(cosf(angle) * INPUT_ACCEL_ONE_G);
            sample.rotation.x = rate * INPUT_ROTATION_ONE_DPS;
            complete |= rest_test_update(&rest, &sample, REST_TEST_DT_US, &mean);
        }
        TEST_ASSERT_FALSE(complete);
    }
}

static void
test_rest_stage_boundaries(void) {
    imu_rest_t rest = {0};
    input_motion_t sample = fixture();
    TEST_ASSERT_EQUAL_INT(IMU_REST_MOVING, imu_rest_step(&rest, &sample, REST_TEST_DT_US, true));
    const uint32_t gap = IMU_REST_MAX_GAP_MS * MATH_US_PER_MS;
    TEST_ASSERT_EQUAL_INT(IMU_REST_SETTLING, imu_rest_step(&rest, &sample, gap, true));
    TEST_ASSERT_EQUAL_UINT(gap, rest.settle_us);
    const imu_rest_t before = rest;
    imu_rest_step(&rest, NULL, 0, false);
    TEST_ASSERT_EQUAL_MEMORY(&before, &rest, sizeof rest);
    TEST_ASSERT_EQUAL_INT(IMU_REST_MOVING, imu_rest_step(&rest, &sample, gap + 1, true));
    TEST_ASSERT_EQUAL_UINT(0, rest.settle_us);
    imu_rest_step(&rest, &sample, REST_TEST_DT_US, true);
    for (uint32_t t = REST_TEST_DT_US; t <= IMU_REST_SETTLE_MS * MATH_US_PER_MS; t += REST_TEST_DT_US) {
        TEST_ASSERT_EQUAL_INT(IMU_REST_SETTLING, imu_rest_step(&rest, &sample, REST_TEST_DT_US, true));
    }
    for (uint32_t t = REST_TEST_DT_US; t < IMU_REST_WINDOW_MS * MATH_US_PER_MS; t += REST_TEST_DT_US) {
        TEST_ASSERT_EQUAL_INT(IMU_REST_MEASURING, imu_rest_step(&rest, &sample, REST_TEST_DT_US, true));
    }
    TEST_ASSERT_EQUAL_INT(IMU_REST_DONE, imu_rest_step(&rest, &sample, REST_TEST_DT_US, true));
    TEST_ASSERT_FALSE(rest.primed);
}

static void
test_rest_settle_excluded_and_negative_rounding(void) {
    imu_rest_t test = {0};
    input_motion_t sample = fixture();
    vec3i_t mean;
    sample.rotation = (vec3i_t){-REST_TEST_BIAS, 0, 0};
    sample.acceleration.x = REST_TEST_ACCEL_NOISE;
    rest_test_update(&test, &sample, REST_TEST_DT_US, &mean);
    run_window(&test, sample, IMU_REST_SETTLE_MS * MATH_US_PER_MS, &mean);
    sample.rotation.x--;
    for (uint32_t t = 0; t < IMU_REST_WINDOW_MS * MATH_US_PER_MS; t += REST_TEST_DT_US) {
        sample.rotation.x = -REST_TEST_BIAS - ((t / REST_TEST_DT_US) % 2);
        sample.acceleration.x = -(int32_t)((t / REST_TEST_DT_US) % 2);
        const bool done = rest_test_update(&test, &sample, REST_TEST_DT_US, &mean);
        TEST_ASSERT_EQUAL(t + REST_TEST_DT_US == IMU_REST_WINDOW_MS * MATH_US_PER_MS, done);
    }
    TEST_ASSERT_EQUAL_INT(-REST_TEST_BIAS - 1, mean.x);
    TEST_ASSERT_EQUAL_INT(-1, imu_rest_mean(&test).acceleration.x);
    imu_rest_step(&test, NULL, 0, false);
    TEST_ASSERT_EQUAL_INT(-REST_TEST_BIAS - 1, imu_rest_mean(&test).rotation.x);
    TEST_ASSERT_EQUAL_INT(IMU_REST_MOVING, imu_rest_step(&test, &sample, REST_TEST_DT_US, true));
    TEST_ASSERT_EQUAL_INT(0, test.n);
}

static void
test_rest_drift_formula(void) {
    const float drift = INPUT_ACCEL_ONE_G
                        * sinf((float)IMU_REST_RATE_RESOLUTION / INPUT_ROTATION_ONE_DPS
                               * (IMU_REST_SETTLE_MS + IMU_REST_WINDOW_MS) / MATH_MS_PER_S * MATH_RADIANS_PER_DEGREE);
    TEST_ASSERT_TRUE(drift == IMU_REST_DRIFT);
}

void
run_imu_rest_suite(void) {
    RUN_TEST(test_rest_drift_formula);
    RUN_TEST(test_rest_stage_boundaries);
    RUN_TEST(test_rest_settle_excluded_and_negative_rounding);
    RUN_TEST(test_rest_smooth_tilt);
    RUN_TEST(test_rest_bias_and_noise);
    RUN_TEST(test_rest_restarts_and_magnitude);
}

SUITE_REGISTER(run_imu_rest_suite);
