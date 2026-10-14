#include "suites.h"
#include "unity.h"

#include <limits.h>

#include "input/imu_calib.h"

static imu_calib_t
fixture(void) {
    return (imu_calib_t){.offset = {INPUT_ACCEL_ONE_G / 8, -INPUT_ACCEL_ONE_G / 16, INPUT_ACCEL_ONE_G / 32}};
}

static void
test_fit_and_apply(void) {
    const imu_calib_t expected = fixture();
    const vec3i_t span = {INPUT_ACCEL_ONE_G, INPUT_ACCEL_ONE_G / 2, 2 * INPUT_ACCEL_ONE_G};
    const imu_calib_fit_t fit = imu_calib_fit(vec3i_add(expected.offset, span), vec3i_sub(expected.offset, span));
    TEST_ASSERT_EQUAL_MEMORY(&expected, &fit.calib, sizeof expected);
    TEST_ASSERT_EQUAL_INT(IMU_CALIB_SCALE_ONE, fit.scale_ppm.x);
    TEST_ASSERT_EQUAL_INT(IMU_CALIB_SCALE_ONE / 2, fit.scale_ppm.y);
    TEST_ASSERT_EQUAL_INT(2 * IMU_CALIB_SCALE_ONE, fit.scale_ppm.z);
    const vec3i_t corrected = imu_calib_apply(&fit.calib, vec3i_add(expected.offset, span));
    TEST_ASSERT_EQUAL_MEMORY(&span, &corrected, sizeof span);
}

static void
test_measured_board_fit(void) {
    const vec3i_t offset = {426, 90, -438};
    const vec3i_t span = {4133, 4084, 4071};
    const imu_calib_fit_t fit = imu_calib_fit(vec3i_add(offset, span), vec3i_sub(offset, span));
    TEST_ASSERT_EQUAL_MEMORY(&offset, &fit.calib.offset, sizeof offset);
    TEST_ASSERT_INT_WITHIN(1, 1009033, fit.scale_ppm.x);
    TEST_ASSERT_INT_WITHIN(1, 997070, fit.scale_ppm.y);
    TEST_ASSERT_INT_WITHIN(1, 993896, fit.scale_ppm.z);
}

static void
test_saved_validation(void) {
    bool found[3] = {true, true, true};
    int32_t saved[3] = {IMU_CALIB_OFFSET_MAX, -IMU_CALIB_OFFSET_MAX, 0};
    const imu_calib_t valid = imu_calib_from_saved(found, saved);
    TEST_ASSERT_EQUAL_INT(saved[0], valid.offset.x);
    TEST_ASSERT_EQUAL_INT(saved[1], valid.offset.y);
    const vec3i_t corrected = imu_calib_apply(&valid, valid.offset);
    TEST_ASSERT_TRUE(vec3i_equal((vec3i_t){0}, corrected));
    const vec3i_t zero = {0};
    for (int axis = 0; axis < 3; axis++) {
        found[axis] = false;
        imu_calib_t result = imu_calib_from_saved(found, saved);
        TEST_ASSERT_EQUAL_MEMORY(&zero, &result.offset, sizeof zero);
        found[axis] = true;
        const int32_t original = saved[axis];
        const int32_t invalid[] = {IMU_CALIB_OFFSET_MAX + 1, -IMU_CALIB_OFFSET_MAX - 1, INT32_MIN, INT32_MAX};
        for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; i++) {
            saved[axis] = invalid[i];
            result = imu_calib_from_saved(found, saved);
            TEST_ASSERT_EQUAL_MEMORY(&zero, &result.offset, sizeof zero);
        }
        saved[axis] = original;
    }
    found[0] = found[1] = found[2] = false;
    const imu_calib_t absent = imu_calib_from_saved(found, saved);
    TEST_ASSERT_EQUAL_MEMORY(&zero, &absent.offset, sizeof zero);
    const vec3i_t raw = fixture().offset;
    const vec3i_t unchanged = imu_calib_apply(&absent, raw);
    TEST_ASSERT_EQUAL_MEMORY(&raw, &unchanged, sizeof raw);
}

void
run_imu_calib_suite(void) {
    RUN_TEST(test_fit_and_apply);
    RUN_TEST(test_measured_board_fit);
    RUN_TEST(test_saved_validation);
}

SUITE_REGISTER(run_imu_calib_suite);
