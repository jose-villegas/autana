#include "suites.h"
#include "unity.h"

#include "apps/input_lab/imu_record.h"
#include "math/scalar/math_const.h"

#define TEST_TOLERANCE_MG 0.01F

static shake_probe_levels_t
fixture(void) {
    return (shake_probe_levels_t){0};
}

static void
test_instantaneous_size(void) {
    const vec3f_t vectors[] = {{-1, 0, 0}, {0, 1, 0}, {0, 0, -1}, {0.6F, -0.8F, 0}};
    for (size_t i = 0; i < sizeof vectors / sizeof vectors[0]; i++) {
        shake_probe_levels_t levels = fixture();
        shake_probe_feed(&levels, vectors[i]);
        TEST_ASSERT_FLOAT_WITHIN(TEST_TOLERANCE_MG, IMU_PROBE_MILLI, levels.size);
        TEST_ASSERT_TRUE(levels.shake.smooth_g > 0);
        TEST_ASSERT_TRUE(levels.shake.smooth_g * IMU_PROBE_MILLI < levels.size);
    }
}

static void
test_advance_remainder_and_flashes(void) {
    shake_probe_round_t round = {0};
    TEST_ASSERT_FALSE(shake_probe_running(&round));
    shake_probe_start(&round);
    TEST_ASSERT_TRUE(shake_probe_running(&round));
    TEST_ASSERT_EQUAL_UINT32(MOTION_STEP_FLASH_MS, round.flash_ms);
    TEST_ASSERT_FALSE(shake_probe_advance(&round, 1));
    TEST_ASSERT_EQUAL_UINT32(MOTION_STEP_FLASH_MS - 1, round.flash_ms);
    TEST_ASSERT_TRUE(shake_probe_advance(&round, SHAKE_PROBE_STEP_MS));
    TEST_ASSERT_EQUAL_INT(SHAKE_PROBE_HELD, round.step);
    TEST_ASSERT_EQUAL_UINT32(1, round.ms);
    TEST_ASSERT_EQUAL_UINT32(MOTION_STEP_FLASH_MS - 1, round.flash_ms);
    TEST_ASSERT_TRUE(shake_probe_advance(&round, 2 * SHAKE_PROBE_STEP_MS));
    TEST_ASSERT_EQUAL_INT(SHAKE_PROBE_HARD, round.step);
    TEST_ASSERT_EQUAL_UINT32(1, round.ms);
    TEST_ASSERT_EQUAL_UINT32(MOTION_STEP_FLASH_MS - 1, round.flash_ms);
    TEST_ASSERT_FALSE(shake_probe_advance(&round, MOTION_STEP_FLASH_MS));
    TEST_ASSERT_EQUAL_UINT32(0, round.flash_ms);
    TEST_ASSERT_TRUE(
        shake_probe_advance(&round, SHAKE_PROBE_ROUND_MS - (3 * SHAKE_PROBE_STEP_MS) - MOTION_STEP_FLASH_MS));
    TEST_ASSERT_FALSE(shake_probe_running(&round));
    TEST_ASSERT_EQUAL_INT(SHAKE_PROBE_DONE, round.step);
    TEST_ASSERT_EQUAL_UINT32(MOTION_DONE_FLASH_MS - 1, round.flash_ms);
    TEST_ASSERT_FALSE(shake_probe_advance(&round, MOTION_DONE_FLASH_MS));
    TEST_ASSERT_EQUAL_UINT32(0, round.flash_ms);
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
    TEST_ASSERT_EQUAL_UINT32(MOTION_DONE_FLASH_MS, round.flash_ms);
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
                                        level, UINT8_MAX));
    TEST_ASSERT_LESS_OR_EQUAL_UINT(IMU_RECORD_SHAKE_LINE_SIZE, record.used);
    TEST_ASSERT_FALSE(record.truncated);
    imu_record_free(&record);
}

void
run_shake_probe_suite(void) {
    RUN_TEST(test_sample_line_bound);
    RUN_TEST(test_instantaneous_size);
    RUN_TEST(test_advance_remainder_and_flashes);
    RUN_TEST(test_step_timing);
}

SUITE_REGISTER(run_shake_probe_suite);
