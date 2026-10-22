#include "suites.h"
#include "unity.h"

#include "apps/input_lab/imu_record.h"
#include "math/scalar/math_const.h"

#define TEST_TOLERANCE_MG 0.01F
#define TEST_OFFSET_G     0.3F
#define TEST_SETTLE_MS    (20U * SHAKE_PROBE_SLOW_MS)
#define TEST_SINE_HZ      4.0F

static shake_probe_levels_t
fixture(void) {
    return (shake_probe_levels_t){0};
}

static void
test_still(void) {
    shake_probe_levels_t levels = fixture();
    shake_probe_feed(&levels, (vec3f_t){0});
    TEST_ASSERT_FLOAT_WITHIN(TEST_TOLERANCE_MG, 0, levels.size);
    TEST_ASSERT_FLOAT_WITHIN(TEST_TOLERANCE_MG, 0, levels.smooth);
    TEST_ASSERT_FLOAT_WITHIN(TEST_TOLERANCE_MG, 0, levels.jerky);
}

static void
test_constant_offset(void) {
    shake_probe_levels_t levels = fixture();
    for (uint32_t us = 0; us < TEST_SETTLE_MS * MATH_US_PER_MS; us += INPUT_MOTION_SAMPLE_US) {
        shake_probe_feed(&levels, (vec3f_t){TEST_OFFSET_G, 0, 0});
    }
    TEST_ASSERT_FLOAT_WITHIN(TEST_TOLERANCE_MG, TEST_OFFSET_G * SHAKE_PROBE_MILLI_G, levels.smooth);
    TEST_ASSERT_FLOAT_WITHIN(TEST_TOLERANCE_MG, 0, levels.jerky);
}

static void
test_sine_axes(void) {
    shake_probe_levels_t across = fixture();
    shake_probe_levels_t along = fixture();
    for (uint32_t us = 0; us < TEST_SETTLE_MS * MATH_US_PER_MS; us += INPUT_MOTION_SAMPLE_US) {
        const float value = TEST_OFFSET_G * sinf(2.0F * MATH_PI * TEST_SINE_HZ * (float)us / MATH_US_PER_S);
        shake_probe_feed(&across, (vec3f_t){value, 0, 0});
        shake_probe_feed(&along, (vec3f_t){0, 0, value});
        TEST_ASSERT_FLOAT_WITHIN(TEST_TOLERANCE_MG, along.smooth, across.smooth);
        TEST_ASSERT_FLOAT_WITHIN(TEST_TOLERANCE_MG, along.jerky, across.jerky);
    }
    TEST_ASSERT_GREATER_THAN(SHAKE_PROBE_MILLI_G * TEST_OFFSET_G / 4, across.smooth);
}

static void
test_step_timing(void) {
    shake_probe_round_t round = {0};
    TEST_ASSERT_FALSE(shake_probe_advance(&round, SHAKE_PROBE_STEP_MS));
    shake_probe_start(&round);
    for (int step = SHAKE_PROBE_STILL; step < SHAKE_PROBE_DONE; step++) {
        TEST_ASSERT_EQUAL_INT(step, round.step);
        TEST_ASSERT_NOT_EQUAL('\0', *shake_probe_prompt(round.step));
        TEST_ASSERT_FALSE(shake_probe_advance(&round, SHAKE_PROBE_STEP_MS - 1));
        TEST_ASSERT_TRUE(shake_probe_advance(&round, 1));
        TEST_ASSERT_EQUAL_UINT32(0, round.ms);
    }
    TEST_ASSERT_EQUAL_INT(SHAKE_PROBE_DONE, round.step);
    TEST_ASSERT_EQUAL_UINT32(SHAKE_PROBE_DONE_FLASH_MS, round.flash_ms);
    TEST_ASSERT_FALSE(shake_probe_advance(&round, SHAKE_PROBE_STEP_MS));
    shake_probe_start(&round);
    TEST_ASSERT_TRUE(shake_probe_advance(&round, SHAKE_PROBE_ROUND_MS + 1));
    TEST_ASSERT_EQUAL_INT(SHAKE_PROBE_DONE, round.step);
}

static void
test_sample_line_bound(void) {
    imu_record_t record = {0};
    TEST_ASSERT_TRUE(imu_record_start(&record, IMU_RECORD_SHAKE_LINE_SIZE + sizeof IMU_RECORD_TRUNCATED));
    const int acc = -SHAKE_PROBE_ACCEL_MAX_MG;
    const int lin = -SHAKE_PROBE_LINEAR_MAX_MG;
    const int level = SHAKE_PROBE_CANDIDATE_MAX_MG;
    TEST_ASSERT_TRUE(imu_record_appendf(&record, SHAKE_PROBE_SAMPLE_FORMAT, SHAKE_PROBE_TAP,
                                        (unsigned)(SHAKE_PROBE_ROUND_MS - 1), acc, acc, acc, lin, lin, lin, level,
                                        level, level, UINT8_MAX));
    TEST_ASSERT_LESS_OR_EQUAL_UINT(IMU_RECORD_SHAKE_LINE_SIZE, record.used);
    TEST_ASSERT_FALSE(record.truncated);
    imu_record_free(&record);
}

void
run_shake_probe_suite(void) {
    RUN_TEST(test_sample_line_bound);
    RUN_TEST(test_still);
    RUN_TEST(test_constant_offset);
    RUN_TEST(test_sine_axes);
    RUN_TEST(test_step_timing);
}

SUITE_REGISTER(run_shake_probe_suite);
