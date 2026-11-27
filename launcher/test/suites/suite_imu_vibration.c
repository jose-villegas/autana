/* imu_vibration: spread of an accelerometer burst around its own mean. */
#include <math.h>

#include "suites.h"
#include "unity.h"

#include "input/motion/imu_vibration.h"

#define TEST_TOLERANCE_MG 0.001F
#define TEST_SWING_COUNTS 410
#define TEST_TILT_COUNTS  1234

static float
mg(float counts) {
    return counts * IMU_VIBRATION_MG_PER_G / (float)IMU_COUNTS_PER_G;
}

static void
test_too_few_samples(void) {
    const imu_sample_t one = {.az = IMU_COUNTS_PER_G};
    imu_vibration_t out = {.samples = -1};
    TEST_ASSERT_FALSE(imu_vibration_measure(&one, IMU_VIBRATION_MIN_SAMPLES - 1, &out));
    TEST_ASSERT_EQUAL_INT(-1, out.samples);
}

static void
test_still_board_reads_zero(void) {
    const imu_sample_t still[] = {{.ax = TEST_TILT_COUNTS, .az = IMU_COUNTS_PER_G},
                                  {.ax = TEST_TILT_COUNTS, .az = IMU_COUNTS_PER_G},
                                  {.ax = TEST_TILT_COUNTS, .az = IMU_COUNTS_PER_G}};
    imu_vibration_t out;
    TEST_ASSERT_TRUE(imu_vibration_measure(still, 3, &out));
    TEST_ASSERT_EQUAL_FLOAT(0, out.rms_mg);
    TEST_ASSERT_EQUAL_FLOAT(0, out.peak_mg);
    TEST_ASSERT_EQUAL_INT(3, out.samples);
    TEST_ASSERT_FLOAT_WITHIN(TEST_TOLERANCE_MG, mg(TEST_TILT_COUNTS), out.gx_mg);
    TEST_ASSERT_EQUAL_FLOAT(0, out.gy_mg);
    TEST_ASSERT_FLOAT_WITHIN(TEST_TOLERANCE_MG, IMU_VIBRATION_MG_PER_G, out.gz_mg);
}

/* Gravity and tilt sit in the mean; a swing on two axes reads as its vector size. */
static void
test_swing_over_gravity(void) {
    const imu_sample_t swing[] = {
        {.ax = TEST_TILT_COUNTS + TEST_SWING_COUNTS, .ay = TEST_SWING_COUNTS, .az = IMU_COUNTS_PER_G},
        {.ax = TEST_TILT_COUNTS - TEST_SWING_COUNTS, .ay = -TEST_SWING_COUNTS, .az = IMU_COUNTS_PER_G},
        {.ax = TEST_TILT_COUNTS + TEST_SWING_COUNTS, .ay = TEST_SWING_COUNTS, .az = IMU_COUNTS_PER_G},
        {.ax = TEST_TILT_COUNTS - TEST_SWING_COUNTS, .ay = -TEST_SWING_COUNTS, .az = IMU_COUNTS_PER_G}};
    imu_vibration_t out;
    TEST_ASSERT_TRUE(imu_vibration_measure(swing, 4, &out));
    const float expected = mg(sqrtf(2.0F) * TEST_SWING_COUNTS);
    TEST_ASSERT_FLOAT_WITHIN(TEST_TOLERANCE_MG, expected, out.rms_mg);
    TEST_ASSERT_FLOAT_WITHIN(TEST_TOLERANCE_MG, expected, out.peak_mg);
    TEST_ASSERT_FLOAT_WITHIN(TEST_TOLERANCE_MG, mg(TEST_TILT_COUNTS), out.gx_mg);
    TEST_ASSERT_EQUAL_FLOAT(0, out.gy_mg);
    TEST_ASSERT_FLOAT_WITHIN(TEST_TOLERANCE_MG, IMU_VIBRATION_MG_PER_G, out.gz_mg);
}

/* One knock among still samples: the peak finds it, the RMS spreads it. */
static void
test_peak_and_rms_differ(void) {
    imu_sample_t knock[4] = {{0}, {0}, {0}, {.az = 4 * TEST_SWING_COUNTS}};
    imu_vibration_t out;
    TEST_ASSERT_TRUE(imu_vibration_measure(knock, 4, &out));
    /* Mean TEST_SWING_COUNTS: three samples sit one swing below it, the knock three above. */
    TEST_ASSERT_EQUAL_FLOAT(0, out.gx_mg);
    TEST_ASSERT_EQUAL_FLOAT(0, out.gy_mg);
    TEST_ASSERT_FLOAT_WITHIN(TEST_TOLERANCE_MG, mg(TEST_SWING_COUNTS), out.gz_mg);
    TEST_ASSERT_FLOAT_WITHIN(TEST_TOLERANCE_MG, mg(3.0F * TEST_SWING_COUNTS), out.peak_mg);
    TEST_ASSERT_FLOAT_WITHIN(TEST_TOLERANCE_MG, mg(sqrtf(3.0F) * TEST_SWING_COUNTS), out.rms_mg);
}

void
run_imu_vibration_suite(void) {
    RUN_TEST(test_too_few_samples);
    RUN_TEST(test_still_board_reads_zero);
    RUN_TEST(test_swing_over_gravity);
    RUN_TEST(test_peak_and_rms_differ);
}

SUITE_REGISTER(run_imu_vibration_suite);
