#include "suites.h"
#include "unity.h"

#include <math.h>
#include <stdlib.h>

#include "apps/input_lab/imu_probe.h"
#include "input/imu_sample.h"
#include "ui/ui_transform.h"

#define TEST_DT_MS 20

static input_motion_t
fixture(void) {
    return (input_motion_t){.acceleration = {0, 0, -INPUT_ACCEL_ONE_G},
                            .rotation = {10 * INPUT_ROTATION_ONE_DPS, -10 * INPUT_ROTATION_ONE_DPS, 0}};
}

static void
test_settle_and_window(void) {
    imu_probe_round_t* round = calloc(1, sizeof *round);
    TEST_ASSERT_NOT_NULL(round);
    imu_probe_start(round, 0);
    const input_motion_t sample = fixture();
    TEST_ASSERT_FALSE(imu_probe_update(round, sample, TEST_DT_MS));
    for (int ms = 0; ms < IMU_PROBE_SETTLE_MS + IMU_PROBE_WINDOW_MS - TEST_DT_MS; ms += TEST_DT_MS) {
        TEST_ASSERT_FALSE(imu_probe_update(round, sample, TEST_DT_MS));
    }
    TEST_ASSERT_TRUE(imu_probe_update(round, sample, TEST_DT_MS));
    TEST_ASSERT_EQUAL_INT(1, round->pose);
    TEST_ASSERT_EQUAL_INT(IMU_PROBE_WINDOW_MS / TEST_DT_MS, round->poses[0].n);
    free(round);
}

static void
test_mapping_and_pose_names(void) {
    const imu_sample_t raw = {.ax = INPUT_ACCEL_ONE_G,
                              .ay = -INPUT_ACCEL_ONE_G,
                              .az = -INPUT_ACCEL_ONE_G,
                              .gx = INPUT_ROTATION_ONE_DPS,
                              .gy = -INPUT_ROTATION_ONE_DPS,
                              .gz = INPUT_ROTATION_ONE_DPS};
    const vec3i_t rot = imu_axes_screen(raw.gx, raw.gy, raw.gz);
    TEST_ASSERT_EQUAL_INT(INPUT_ROTATION_ONE_DPS, rot.x);
    TEST_ASSERT_EQUAL_INT(raw.gx, rot.y);
    TEST_ASSERT_EQUAL_INT(raw.gz, rot.z);
    const vec3i_t accel = imu_axes_screen(raw.ax, raw.ay, raw.az);
    TEST_ASSERT_EQUAL_INT(imu_gravity_screen(&raw).x, accel.x);
    TEST_ASSERT_EQUAL_INT(imu_gravity_screen(&raw).y, accel.y);
    TEST_ASSERT_EQUAL_INT(raw.az, accel.z);
    const vec3i_t logical[] = {{0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}};
    const char* names[] = {"face_up", "face_down", "left_down", "right_down", "top_down", "bottom_down"};
    for (int q = 0; q < 4; q++) {
        for (int p = 0; p < IMU_PROBE_POSE_COUNT; p++) {
            const vec3i_t d = imu_probe_pose_direction(p, q);
            int x, y;
            ui_transform_point(ui_transform_quarter_turn(-q, 0, 0), d.x, d.y, &x, &y);
            TEST_ASSERT_EQUAL_INT(logical[p].x, x);
            TEST_ASSERT_EQUAL_INT(logical[p].y, y);
            TEST_ASSERT_EQUAL_INT(logical[p].z, d.z);
            TEST_ASSERT_EQUAL_STRING(names[p], imu_probe_pose_name(p, true));
        }
    }
}

static void
check_departure(int axis, bool gyro) {
    for (int side = -1; side <= 1; side++) {
        imu_probe_round_t* round = calloc(1, sizeof *round);
        TEST_ASSERT_NOT_NULL(round);
        imu_probe_start(round, 0);
        input_motion_t sample = fixture();
        imu_probe_update(round, sample, TEST_DT_MS);
        const int departure = (gyro ? IMU_PROBE_GYRO_DEPARTURE : IMU_PROBE_ACCEL_DEPARTURE) + side;
        const vec3i_t delta = {axis == 0 ? departure : 0, axis == 1 ? departure : 0, axis == 2 ? departure : 0};
        if (gyro) {
            sample.rotation = vec3i_add(sample.rotation, delta);
        } else {
            sample.acceleration = vec3i_add(sample.acceleration, delta);
        }
        imu_probe_update(round, sample, TEST_DT_MS);
        TEST_ASSERT_EQUAL_UINT32(side < 0 ? TEST_DT_MS : 0, round->settle_ms);
        free(round);
    }
}

static void
test_departure_boundaries(void) {
    for (int axis = 0; axis < 3; axis++) {
        check_departure(axis, false);
        check_departure(axis, true);
    }
}

static void
test_gravity_gate_and_wrong_pose(void) {
    for (int side = -1; side <= 1; side++) {
        imu_probe_round_t* round = calloc(1, sizeof *round);
        TEST_ASSERT_NOT_NULL(round);
        imu_probe_start(round, 0);
        input_motion_t sample = fixture();
        sample.acceleration.z = -(IMU_PROBE_GRAVITY_MIN + side);
        imu_probe_update(round, sample, TEST_DT_MS);
        imu_probe_update(round, sample, TEST_DT_MS);
        TEST_ASSERT_EQUAL_UINT32(side < 0 ? 0 : TEST_DT_MS, round->settle_ms);
        free(round);
    }
    imu_probe_round_t* round = calloc(1, sizeof *round);
    TEST_ASSERT_NOT_NULL(round);
    imu_probe_start(round, 0);
    input_motion_t sample = fixture();
    sample.acceleration.z = INPUT_ACCEL_ONE_G;
    for (int ms = 0; ms < IMU_PROBE_SETTLE_MS + IMU_PROBE_WINDOW_MS; ms += TEST_DT_MS) {
        TEST_ASSERT_FALSE(imu_probe_update(round, sample, TEST_DT_MS));
    }
    TEST_ASSERT_EQUAL_UINT32(0, round->settle_ms);
    free(round);
}

static void
test_movement_gap_restart_and_rounding(void) {
    imu_probe_round_t* round = calloc(1, sizeof *round);
    TEST_ASSERT_NOT_NULL(round);
    imu_probe_start(round, 0);
    const input_motion_t sample = fixture();
    for (int ms = 0; ms < IMU_PROBE_SETTLE_MS + 2 * TEST_DT_MS; ms += TEST_DT_MS) {
        imu_probe_update(round, sample, TEST_DT_MS);
    }
    TEST_ASSERT_TRUE(round->window.n > 0);
    input_motion_t moved = sample;
    moved.rotation.x += IMU_PROBE_GYRO_DEPARTURE;
    imu_probe_update(round, moved, TEST_DT_MS);
    TEST_ASSERT_EQUAL_INT(0, round->window.n);
    TEST_ASSERT_EQUAL_UINT32(0, round->settle_ms);
    imu_probe_update(round, sample, IMU_PROBE_MAX_GAP_MS + 1);
    TEST_ASSERT_EQUAL_UINT32(0, round->settle_ms);
    imu_probe_start(round, 1);
    TEST_ASSERT_EQUAL_INT(0, round->pose);
    TEST_ASSERT_FALSE(round->primed);
    free(round);
    imu_probe_average_t average = {0};
    imu_probe_average_add(&average, (input_motion_t){0});
    imu_probe_average_add(&average, (input_motion_t){.acceleration = {1, -1, 1}, .rotation = {-1, 1, -1}});
    const input_motion_t mean = imu_probe_average_read(&average);
    TEST_ASSERT_EQUAL_INT(1, mean.acceleration.x);
    TEST_ASSERT_EQUAL_INT(-1, mean.acceleration.y);
    TEST_ASSERT_EQUAL_INT(-1, mean.rotation.x);
    TEST_ASSERT_EQUAL_INT(1, mean.rotation.y);
}

static void
test_synthetic_fit(void) {
    const int offset[] = {INPUT_ACCEL_ONE_G / 16, -INPUT_ACCEL_ONE_G / 8, INPUT_ACCEL_ONE_G / 32};
    const int span[] = {INPUT_ACCEL_ONE_G + INPUT_ACCEL_ONE_G / 16, INPUT_ACCEL_ONE_G - INPUT_ACCEL_ONE_G / 8,
                        INPUT_ACCEL_ONE_G + INPUT_ACCEL_ONE_G / 32};
    for (int q = 0; q < 4; q++) {
        imu_probe_round_t* round = calloc(1, sizeof *round);
        TEST_ASSERT_NOT_NULL(round);
        imu_probe_start(round, q);
        for (int pose = 0; pose < IMU_PROBE_POSE_COUNT; pose++) {
            const vec3i_t d = imu_probe_pose_direction(pose, q);
            input_motion_t sample = fixture();
            sample.acceleration =
                (vec3i_t){offset[0] + d.x * span[0], offset[1] + d.y * span[1], offset[2] + d.z * span[2]};
            imu_probe_average_add(&round->poses[pose], sample);
        }
        round->fit = imu_probe_fit(round->poses, q);
        const int got_off[] = {round->fit.offset_mg.x, round->fit.offset_mg.y, round->fit.offset_mg.z};
        const int got_scale[] = {round->fit.scale_ppm.x, round->fit.scale_ppm.y, round->fit.scale_ppm.z};
        for (int axis = 0; axis < 3; axis++) {
            TEST_ASSERT_EQUAL_INT((int)lround((double)offset[axis] * IMU_PROBE_MILLI / INPUT_ACCEL_ONE_G),
                                  got_off[axis]);
            TEST_ASSERT_EQUAL_INT((int)lround((double)span[axis] * IMU_PROBE_SCALE_ONE / INPUT_ACCEL_ONE_G),
                                  got_scale[axis]);
        }
        TEST_ASSERT_EQUAL_INT(10 * IMU_PROBE_MILLI, round->fit.gyro_mdps.x);
        TEST_ASSERT_EQUAL_INT(-10 * IMU_PROBE_MILLI, round->fit.gyro_mdps.y);
        free(round);
    }
}

static void
test_complete_round(void) {
    imu_probe_round_t* round = calloc(1, sizeof *round);
    TEST_ASSERT_NOT_NULL(round);
    for (int q = 0; q < 4; q++) {
        imu_probe_start(round, q);
        for (int pose = 0; pose < IMU_PROBE_POSE_COUNT; pose++) {
            input_motion_t sample = fixture();
            sample.acceleration = vec3i_scale(imu_probe_pose_direction(pose, q), INPUT_ACCEL_ONE_G);
            for (int ms = 0; ms < IMU_PROBE_SETTLE_MS + IMU_PROBE_WINDOW_MS; ms += TEST_DT_MS) {
                TEST_ASSERT_FALSE(imu_probe_update(round, sample, TEST_DT_MS));
            }
            TEST_ASSERT_TRUE(imu_probe_update(round, sample, TEST_DT_MS));
        }
        TEST_ASSERT_FALSE(round->active);
        TEST_ASSERT_EQUAL_INT(IMU_PROBE_POSE_COUNT, round->pose);
        TEST_ASSERT_EQUAL_INT(IMU_PROBE_SCALE_ONE, round->fit.scale_ppm.x);
        TEST_ASSERT_EQUAL_INT(0, round->fit.offset_mg.x);
        TEST_ASSERT_FALSE(imu_probe_update(round, fixture(), TEST_DT_MS));
    }
    free(round);
}

void
run_imu_probe_suite(void) {
    RUN_TEST(test_settle_and_window);
    RUN_TEST(test_mapping_and_pose_names);
    RUN_TEST(test_departure_boundaries);
    RUN_TEST(test_gravity_gate_and_wrong_pose);
    RUN_TEST(test_movement_gap_restart_and_rounding);
    RUN_TEST(test_synthetic_fit);
    RUN_TEST(test_complete_round);
}

SUITE_REGISTER(run_imu_probe_suite);
