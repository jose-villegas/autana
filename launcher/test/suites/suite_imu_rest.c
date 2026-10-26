#include "input/motion/imu_rest.h"
#include "suites.h"
#include "unity.h"

#define REST_TEST_DT_US       (10U * MATH_US_PER_MS)
#define REST_TEST_BIAS        (4 * INPUT_ROTATION_ONE_DPS)
#define REST_TEST_DURATION_US ((IMU_REST_SETTLE_MS + IMU_REST_WINDOW_MS) * MATH_US_PER_MS)

static input_motion_t
fixture(void) {
    return (input_motion_t){.acceleration = {0, 0, -INPUT_ACCEL_ONE_G}, .rotation = {REST_TEST_BIAS, 0, 0}};
}

static bool
run_window(imu_rest_t* rest, input_motion_t sample, uint32_t duration, vec3i_t* mean) {
    bool complete = false;
    for (uint32_t t = 0; t < duration; t += REST_TEST_DT_US) {
        complete |= imu_rest_update(rest, &sample, REST_TEST_DT_US, mean);
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
            complete |= imu_rest_update(&rest, &sample, REST_TEST_DT_US, &mean);
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
        TEST_ASSERT_FALSE(imu_rest_update(&rest, &sample, dt, &mean));
        TEST_ASSERT_FALSE(run_window(&rest, fixture(), IMU_REST_WINDOW_MS * MATH_US_PER_MS, &mean));
        TEST_ASSERT_TRUE(run_window(&rest, fixture(), REST_TEST_DURATION_US, &mean));
    }
    imu_rest_t rest = {0};
    input_motion_t sample = fixture();
    sample.acceleration.z *= 2;
    vec3i_t mean;
    TEST_ASSERT_FALSE(run_window(&rest, sample, REST_TEST_DURATION_US * 2, &mean));
}

void
run_imu_rest_suite(void) {
    RUN_TEST(test_rest_bias_and_noise);
    RUN_TEST(test_rest_restarts_and_magnitude);
}

SUITE_REGISTER(run_imu_rest_suite);
