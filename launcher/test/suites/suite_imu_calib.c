#include "suites.h"
#include "unity.h"

#include <limits.h>

#include "input/motion/imu_calib.h"
#include "input/motion/imu_rest.h"

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
    TEST_ASSERT_EQUAL_INT(1009033, fit.scale_ppm.x);
    TEST_ASSERT_EQUAL_INT(997070, fit.scale_ppm.y);
    TEST_ASSERT_EQUAL_INT(993897, fit.scale_ppm.z);
}

static void
test_fit_rounding(void) {
    for (int sign = -1; sign <= 1; sign += 2) {
        const vec3i_t expected = {2 * sign, 2 * sign, 2 * sign};
        const vec3i_t up = {INPUT_ACCEL_ONE_G + 3 * sign, INPUT_ACCEL_ONE_G + 3 * sign, INPUT_ACCEL_ONE_G + 3 * sign};
        const vec3i_t down = {-INPUT_ACCEL_ONE_G, -INPUT_ACCEL_ONE_G, -INPUT_ACCEL_ONE_G};
        const imu_calib_fit_t fit = imu_calib_fit(up, down);
        TEST_ASSERT_EQUAL_MEMORY(&expected, &fit.calib.offset, sizeof expected);
    }
}

static void
test_fit_flipped(void) {
    const imu_calib_t expected = fixture();
    const vec3i_t span = {INPUT_ACCEL_ONE_G, INPUT_ACCEL_ONE_G, INPUT_ACCEL_ONE_G};
    const imu_calib_fit_t fit = imu_calib_fit(vec3i_sub(expected.offset, span), vec3i_add(expected.offset, span));
    TEST_ASSERT_EQUAL_MEMORY(&expected, &fit.calib.offset, sizeof expected);
    TEST_ASSERT_EQUAL_INT(-IMU_CALIB_SCALE_ONE, fit.scale_ppm.x);
    TEST_ASSERT_EQUAL_INT(-IMU_CALIB_SCALE_ONE, fit.scale_ppm.y);
    TEST_ASSERT_EQUAL_INT(-IMU_CALIB_SCALE_ONE, fit.scale_ppm.z);
}

static void
test_saved_requires_commit(void) {
    const bool found[] = {true, true, true};
    const int32_t saved[] = {1, -2, 3};
    imu_calib_t uncommitted;
    TEST_ASSERT_FALSE(imu_calib_from_saved(false, found, saved, IMU_CALIB_OFFSET_MAX, &uncommitted));
    TEST_ASSERT_TRUE(vec3i_equal((vec3i_t){0}, uncommitted.offset));
    imu_calib_t committed;
    TEST_ASSERT_TRUE(imu_calib_from_saved(true, found, saved, IMU_CALIB_OFFSET_MAX, &committed));
    TEST_ASSERT_TRUE(vec3i_equal((vec3i_t){1, -2, 3}, committed.offset));
}

static void
test_saved_validation(void) {
    bool found[3] = {true, true, true};
    int32_t saved[3] = {IMU_CALIB_OFFSET_MAX, -IMU_CALIB_OFFSET_MAX, 0};
    imu_calib_t valid;
    TEST_ASSERT_TRUE(imu_calib_from_saved(true, found, saved, IMU_CALIB_OFFSET_MAX, &valid));
    TEST_ASSERT_EQUAL_INT(saved[0], valid.offset.x);
    TEST_ASSERT_EQUAL_INT(saved[1], valid.offset.y);
    const vec3i_t corrected = imu_calib_apply(&valid, valid.offset);
    TEST_ASSERT_TRUE(vec3i_equal((vec3i_t){0}, corrected));
    const vec3i_t zero = {0};
    for (int axis = 0; axis < 3; axis++) {
        found[axis] = false;
        imu_calib_t result;
        imu_calib_from_saved(true, found, saved, IMU_CALIB_OFFSET_MAX, &result);
        TEST_ASSERT_EQUAL_MEMORY(&zero, &result.offset, sizeof zero);
        found[axis] = true;
        const int32_t original = saved[axis];
        const int32_t invalid[] = {IMU_CALIB_OFFSET_MAX + 1, -IMU_CALIB_OFFSET_MAX - 1, INT32_MIN, INT32_MAX};
        for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; i++) {
            saved[axis] = invalid[i];
            imu_calib_from_saved(true, found, saved, IMU_CALIB_OFFSET_MAX, &result);
            TEST_ASSERT_EQUAL_MEMORY(&zero, &result.offset, sizeof zero);
        }
        saved[axis] = original;
    }
    found[0] = found[1] = found[2] = false;
    imu_calib_t absent;
    imu_calib_from_saved(true, found, saved, IMU_CALIB_OFFSET_MAX, &absent);
    TEST_ASSERT_EQUAL_MEMORY(&zero, &absent.offset, sizeof zero);
    const vec3i_t raw = fixture().offset;
    const vec3i_t unchanged = imu_calib_apply(&absent, raw);
    TEST_ASSERT_EQUAL_MEMORY(&raw, &unchanged, sizeof raw);
}

static void
test_gyro_saved_bounds(void) {
    const bool complete[] = {true, true, true};
    for (int axis = 0; axis < 3; axis++) {
        bool found[] = {true, true, true};
        int32_t values[3] = {IMU_CALIB_GYRO_MAX, -IMU_CALIB_GYRO_MAX, IMU_CALIB_GYRO_MAX};
        imu_calib_t calib;
        imu_calib_from_saved(true, complete, values, IMU_CALIB_GYRO_MAX, &calib);
        TEST_ASSERT_EQUAL_INT(values[axis], ((int32_t[]){calib.offset.x, calib.offset.y, calib.offset.z})[axis]);
        found[axis] = false;
        imu_calib_from_saved(true, found, values, IMU_CALIB_GYRO_MAX, &calib);
        TEST_ASSERT_TRUE(vec3i_equal((vec3i_t){0}, calib.offset));
        values[axis] = IMU_CALIB_GYRO_MAX + 1;
        imu_calib_from_saved(true, complete, values, IMU_CALIB_GYRO_MAX, &calib);
        TEST_ASSERT_TRUE(vec3i_equal((vec3i_t){0}, calib.offset));
        imu_calib_from_saved(false, complete, values, IMU_CALIB_GYRO_MAX, &calib);
        TEST_ASSERT_TRUE(vec3i_equal((vec3i_t){0}, calib.offset));
    }
}

static void
test_gyro_first_small_save(void) {
    imu_calib_gyro_t gyro = {0};
    TEST_ASSERT_EQUAL_INT(IMU_CALIB_GYRO_SAVE_DUE, imu_calib_gyro_learn(&gyro, (vec3i_t){1, 0, 0}, 0));
}

static vec3i_t
axis_value(int axis, int value) {
    return (vec3i_t){axis == 0 ? value : 0, axis == 1 ? value : 0, axis == 2 ? value : 0};
}

static void
test_gyro_boundaries(void) {
    for (int axis = 0; axis < 3; axis++) {
        for (int sign = -1; sign <= 1; sign += 2) {
            imu_calib_gyro_t gyro = {0};
            const vec3i_t maximum = axis_value(axis, sign * IMU_CALIB_GYRO_MAX);
            TEST_ASSERT_EQUAL_INT(IMU_CALIB_GYRO_REJECTED,
                                  imu_calib_gyro_learn(&gyro, axis_value(axis, sign * (IMU_CALIB_GYRO_MAX + 1)), 0));
            TEST_ASSERT_TRUE(vec3i_equal((vec3i_t){0}, gyro.live.offset));
            TEST_ASSERT_FALSE(gyro.valid);
            TEST_ASSERT_EQUAL_INT(IMU_CALIB_GYRO_SAVE_DUE, imu_calib_gyro_learn(&gyro, maximum, 0));
            TEST_ASSERT_EQUAL_INT(IMU_CALIB_GYRO_REJECTED, imu_calib_gyro_learn(&gyro, axis_value(axis, sign), 0));
            TEST_ASSERT_TRUE(vec3i_equal(maximum, gyro.live.offset));
            gyro = (imu_calib_gyro_t){.valid = true, .storage_valid = true};
            TEST_ASSERT_EQUAL_INT(
                IMU_CALIB_GYRO_REJECTED,
                imu_calib_gyro_learn(&gyro, axis_value(axis, sign * (IMU_CALIB_GYRO_RESIDUAL_MAX + 1)), 0));
            TEST_ASSERT_TRUE(vec3i_equal((vec3i_t){0}, gyro.live.offset));
            TEST_ASSERT_EQUAL_INT(IMU_CALIB_GYRO_SAVE_DUE,
                                  imu_calib_gyro_learn(&gyro, axis_value(axis, sign * IMU_CALIB_GYRO_RESIDUAL_MAX), 0));
            gyro = (imu_calib_gyro_t){.valid = true, .storage_valid = true};
            imu_calib_gyro_saved(&gyro, true, 0);
            TEST_ASSERT_EQUAL_INT(IMU_CALIB_GYRO_APPLIED,
                                  imu_calib_gyro_learn(&gyro, axis_value(axis, sign * (IMU_CALIB_GYRO_SAVE_DELTA + 1)),
                                                       IMU_CALIB_GYRO_SAVE_INTERVAL_US - 1));
            TEST_ASSERT_EQUAL_INT(IMU_CALIB_GYRO_APPLIED, imu_calib_gyro_learn(&gyro, axis_value(axis, -sign),
                                                                               IMU_CALIB_GYRO_SAVE_INTERVAL_US));
            TEST_ASSERT_EQUAL_INT(IMU_CALIB_GYRO_SAVE_DUE,
                                  imu_calib_gyro_learn(&gyro, axis_value(axis, sign), IMU_CALIB_GYRO_SAVE_INTERVAL_US));
        }
    }
    TEST_ASSERT_FALSE(imu_calib_valid((vec3i_t){IMU_CALIB_OFFSET_MAX, 0, 0}, IMU_CALIB_GYRO_MAX));
    TEST_ASSERT_FALSE(imu_calib_valid((vec3i_t){INPUT_ACCEL_ONE_G, 0, 0}, IMU_CALIB_OFFSET_MAX));
    TEST_ASSERT_TRUE(imu_calib_valid((vec3i_t){IMU_CALIB_GYRO_MAX, 0, 0}, IMU_CALIB_OFFSET_MAX));
}

static void
test_gyro_failed_save_retries_and_throttles(void) {
    imu_calib_gyro_t gyro = {
        .live = {.offset = {IMU_CALIB_GYRO_SAVE_DELTA * 2, 0, 0}}, .valid = true, .storage_valid = true};
    imu_calib_gyro_saved(&gyro, true, 0);
    const int64_t attempt = IMU_CALIB_GYRO_SAVE_INTERVAL_US;
    TEST_ASSERT_EQUAL_INT(IMU_CALIB_GYRO_SAVE_DUE,
                          imu_calib_gyro_learn(&gyro, (vec3i_t){IMU_CALIB_GYRO_SAVE_DELTA + 1, 0, 0}, attempt));
    imu_calib_gyro_saved(&gyro, false, attempt);
    TEST_ASSERT_FALSE(gyro.storage_valid);
    TEST_ASSERT_EQUAL_INT(IMU_CALIB_GYRO_APPLIED,
                          imu_calib_gyro_learn(&gyro, (vec3i_t){0}, attempt + IMU_REST_WINDOW_MS * MATH_US_PER_MS));
    TEST_ASSERT_EQUAL_INT(
        IMU_CALIB_GYRO_APPLIED,
        imu_calib_gyro_learn(&gyro, (vec3i_t){-IMU_CALIB_GYRO_SAVE_DELTA - 1, 0, 0}, attempt * 2 - 1));
    TEST_ASSERT_EQUAL_INT(IMU_CALIB_GYRO_SAVE_DUE, imu_calib_gyro_learn(&gyro, (vec3i_t){0}, attempt * 2));
    imu_calib_gyro_saved(&gyro, true, attempt * 2);
    TEST_ASSERT_TRUE(gyro.storage_valid);
    TEST_ASSERT_EQUAL_INT(IMU_CALIB_GYRO_APPLIED, imu_calib_gyro_learn(&gyro, (vec3i_t){0}, attempt * 3));
}

static void
test_gyro_constructor(void) {
    const int32_t saved[] = {IMU_CALIB_GYRO_MAX, -IMU_CALIB_GYRO_MAX, 0};
    for (int state = 0; state < 3; state++) {
        const bool found[] = {true, state != 1, true};
        const imu_calib_gyro_t gyro = imu_calib_gyro_from_saved(state != 2, found, saved);
        TEST_ASSERT_EQUAL(state == 0, gyro.valid);
        TEST_ASSERT_EQUAL(gyro.valid, gyro.storage_valid);
        TEST_ASSERT_TRUE(vec3i_equal(gyro.live.offset, gyro.saved));
        TEST_ASSERT_TRUE(vec3i_equal(state == 0 ? (vec3i_t){saved[0], saved[1], saved[2]} : (vec3i_t){0}, gyro.saved));
        TEST_ASSERT_EQUAL_INT64(0, gyro.next_save_us);
    }
}

void
run_imu_calib_suite(void) {
    RUN_TEST(test_gyro_constructor);
    RUN_TEST(test_gyro_boundaries);
    RUN_TEST(test_gyro_failed_save_retries_and_throttles);
    RUN_TEST(test_gyro_first_small_save);
    RUN_TEST(test_gyro_saved_bounds);
    RUN_TEST(test_fit_and_apply);
    RUN_TEST(test_measured_board_fit);
    RUN_TEST(test_saved_validation);
    RUN_TEST(test_fit_rounding);
    RUN_TEST(test_fit_flipped);
    RUN_TEST(test_saved_requires_commit);
}

SUITE_REGISTER(run_imu_calib_suite);
