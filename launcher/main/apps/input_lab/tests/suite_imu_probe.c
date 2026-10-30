#include "suites.h"
#include "unity.h"

#include <stdlib.h>

#include "apps/input_lab/imu_probe.h"
#include "display/display.h"
#include "input/motion/imu_sample.h"
#include "math/scalar/mathf_round.h"

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
    TEST_ASSERT_FALSE(imu_probe_update(round, &sample, TEST_DT_MS));
    for (int ms = 0; ms < IMU_REST_SETTLE_MS + IMU_REST_WINDOW_MS - TEST_DT_MS; ms += TEST_DT_MS) {
        TEST_ASSERT_FALSE(imu_probe_update(round, &sample, TEST_DT_MS));
    }
    TEST_ASSERT_TRUE(imu_probe_update(round, &sample, TEST_DT_MS));
    TEST_ASSERT_EQUAL_INT(1, round->pose);
    TEST_ASSERT_EQUAL_INT(IMU_REST_WINDOW_MS / TEST_DT_MS, round->poses[0].n);
    free(round);
}

static void
test_mapping_and_pose_names(void) {
    const imu_sample_t raw = {.ax = INPUT_ACCEL_ONE_G,
                              .ay = 2 * INPUT_ACCEL_ONE_G,
                              .az = 3 * INPUT_ACCEL_ONE_G,
                              .gx = -4 * INPUT_ROTATION_ONE_DPS,
                              .gy = 5 * INPUT_ROTATION_ONE_DPS,
                              .gz = -6 * INPUT_ROTATION_ONE_DPS};
    const vec3i_t rot = imu_axes_screen(raw.gx, raw.gy, raw.gz);
    TEST_ASSERT_EQUAL_INT(-5 * INPUT_ROTATION_ONE_DPS, rot.x);
    TEST_ASSERT_EQUAL_INT(-4 * INPUT_ROTATION_ONE_DPS, rot.y);
    TEST_ASSERT_EQUAL_INT(-6 * INPUT_ROTATION_ONE_DPS, rot.z);
    const vec3i_t accel = imu_axes_screen(raw.ax, raw.ay, raw.az);
    TEST_ASSERT_EQUAL_INT(-2 * INPUT_ACCEL_ONE_G, accel.x);
    TEST_ASSERT_EQUAL_INT(INPUT_ACCEL_ONE_G, accel.y);
    TEST_ASSERT_EQUAL_INT(3 * INPUT_ACCEL_ONE_G, accel.z);
    static const vec3i_t expected[][IMU_PROBE_POSE_COUNT] = {
        {{0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}},
        {{0, 0, -1}, {0, 0, 1}, {0, -1, 0}, {0, 1, 0}, {1, 0, 0}, {-1, 0, 0}},
        {{0, 0, -1}, {0, 0, 1}, {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}},
        {{0, 0, -1}, {0, 0, 1}, {0, 1, 0}, {0, -1, 0}, {-1, 0, 0}, {1, 0, 0}}};
    static const char* const names[] = {"face_up", "face_down", "left_down", "right_down", "top_down", "bottom_down"};
    static const char* const labels[] = {"FACE UP", "FACE DOWN", "LEFT DOWN", "RIGHT DOWN", "TOP DOWN", "BOTTOM DOWN"};
    TEST_ASSERT_EQUAL_INT(1, DISPLAY_DEFAULT_QUARTER);
    for (int q = 0; q < 4; q++) {
        for (int p = 0; p < IMU_PROBE_POSE_COUNT; p++) {
            const vec3i_t d = imu_probe_pose_direction(p, q);
            TEST_ASSERT_EQUAL_INT(expected[q][p].x, d.x);
            TEST_ASSERT_EQUAL_INT(expected[q][p].y, d.y);
            TEST_ASSERT_EQUAL_INT(expected[q][p].z, d.z);
            TEST_ASSERT_EQUAL_STRING(names[p], imu_probe_pose_name(p));
            char label[IMU_PROBE_LABEL_SIZE];
            imu_probe_pose_label(p, label, sizeof label);
            TEST_ASSERT_EQUAL_STRING(labels[p], label);
        }
    }
}

static void
check_departure_sample(int axis, bool gyro, int sign, int side) {
    imu_probe_round_t* round = calloc(1, sizeof *round);
    TEST_ASSERT_NOT_NULL(round);
    imu_probe_start(round, 0);
    input_motion_t sample = fixture();
    imu_probe_update(round, &sample, TEST_DT_MS);
    const int departure = sign * ((gyro ? IMU_REST_GYRO_DEPARTURE : IMU_REST_ACCEL_DEPARTURE) + side);
    const vec3i_t delta = {axis == 0 ? departure : 0, axis == 1 ? departure : 0, axis == 2 ? departure : 0};
    if (gyro) {
        sample.rotation = vec3i_add(sample.rotation, delta);
    } else {
        sample.acceleration = vec3i_add(sample.acceleration, delta);
    }
    imu_probe_update(round, &sample, TEST_DT_MS);
    TEST_ASSERT_EQUAL_UINT32(side < 0 ? TEST_DT_MS : 0, (round->rest.settle_us / MATH_US_PER_MS));
    free(round);
}

static void
check_departure(int axis, bool gyro) {
    for (int sign = -1; sign <= 1; sign += 2) {
        for (int side = -1; side <= 1; side++) {
            check_departure_sample(axis, gyro, sign, side);
        }
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
        imu_probe_update(round, &sample, TEST_DT_MS);
        imu_probe_update(round, &sample, TEST_DT_MS);
        TEST_ASSERT_EQUAL_UINT32(side < 0 ? 0 : TEST_DT_MS, (round->rest.settle_us / MATH_US_PER_MS));
        free(round);
    }
    imu_probe_round_t* round = calloc(1, sizeof *round);
    TEST_ASSERT_NOT_NULL(round);
    imu_probe_start(round, 0);
    input_motion_t sample = fixture();
    sample.acceleration.z = INPUT_ACCEL_ONE_G;
    for (int ms = 0; ms < IMU_REST_SETTLE_MS + IMU_REST_WINDOW_MS; ms += TEST_DT_MS) {
        TEST_ASSERT_FALSE(imu_probe_update(round, &sample, TEST_DT_MS));
    }
    TEST_ASSERT_EQUAL_UINT32(0, (round->rest.settle_us / MATH_US_PER_MS));
    free(round);
}

static void
test_movement_resets_window(void) {
    imu_probe_round_t* round = calloc(1, sizeof *round);
    TEST_ASSERT_NOT_NULL(round);
    imu_probe_start(round, 0);
    input_motion_t sample = fixture();
    for (int ms = 0; ms < IMU_REST_SETTLE_MS + (2 * TEST_DT_MS); ms += TEST_DT_MS) {
        imu_probe_update(round, &sample, TEST_DT_MS);
    }
    TEST_ASSERT_TRUE(round->rest.n > 0);
    sample.rotation.x += IMU_REST_GYRO_DEPARTURE;
    imu_probe_update(round, &sample, TEST_DT_MS);
    TEST_ASSERT_EQUAL_INT(0, round->rest.n);
    TEST_ASSERT_EQUAL_UINT32(0, (round->rest.settle_us / MATH_US_PER_MS));
    free(round);
}

static void
test_gap_boundary(void) {
    imu_probe_round_t* round = calloc(1, sizeof *round);
    TEST_ASSERT_NOT_NULL(round);
    const input_motion_t sample = fixture();
    imu_probe_start(round, 0);
    imu_probe_update(round, &sample, TEST_DT_MS);
    imu_probe_update(round, &sample, IMU_REST_MAX_GAP_MS);
    TEST_ASSERT_EQUAL_UINT32(IMU_REST_MAX_GAP_MS, (round->rest.settle_us / MATH_US_PER_MS));
    imu_probe_update(round, &sample, IMU_REST_MAX_GAP_MS + 1);
    TEST_ASSERT_EQUAL_UINT32(0, (round->rest.settle_us / MATH_US_PER_MS));
    free(round);
}

static void
test_zero_dt(void) {
    imu_probe_round_t* round = calloc(1, sizeof *round);
    TEST_ASSERT_NOT_NULL(round);
    imu_probe_start(round, 0);
    const input_motion_t sample = fixture();
    imu_probe_update(round, &sample, TEST_DT_MS);
    imu_probe_update(round, &sample, TEST_DT_MS);
    const imu_probe_round_t before = *round;
    imu_probe_update(round, NULL, 0);
    imu_probe_update(round, &sample, 0);
    TEST_ASSERT_EQUAL_MEMORY(&before, round, sizeof before);
    free(round);
}

static void
test_missing_reading(void) {
    imu_probe_round_t* round = calloc(1, sizeof *round);
    TEST_ASSERT_NOT_NULL(round);
    imu_probe_start(round, 0);
    const input_motion_t sample = fixture();
    for (int ms = 0; ms < IMU_REST_SETTLE_MS + (2 * TEST_DT_MS); ms += TEST_DT_MS) {
        imu_probe_update(round, &sample, TEST_DT_MS);
    }
    TEST_ASSERT_TRUE(round->rest.n > 0);
    imu_probe_update(round, NULL, TEST_DT_MS);
    TEST_ASSERT_FALSE(round->rest.primed);
    TEST_ASSERT_EQUAL_INT(0, round->rest.n);
    imu_probe_update(round, &sample, TEST_DT_MS);
    TEST_ASSERT_EQUAL_UINT32(0, (round->rest.settle_us / MATH_US_PER_MS));
    TEST_ASSERT_TRUE(round->rest.primed);
    free(round);
}

static void
test_restart(void) {
    imu_probe_round_t* round = calloc(1, sizeof *round);
    TEST_ASSERT_NOT_NULL(round);
    imu_probe_start(round, 0);
    const input_motion_t sample = fixture();
    imu_probe_update(round, &sample, TEST_DT_MS);
    imu_probe_start(round, DISPLAY_DEFAULT_QUARTER);
    TEST_ASSERT_EQUAL_INT(0, round->pose);
    TEST_ASSERT_EQUAL_INT(DISPLAY_DEFAULT_QUARTER, round->quarter);
    TEST_ASSERT_FALSE(round->rest.primed);
    free(round);
}

static void
test_average_empty(void) {
    const imu_rest_t average = {0};
    const input_motion_t mean = imu_rest_mean(&average);
    TEST_ASSERT_EQUAL_INT(0, mean.acceleration.x);
    TEST_ASSERT_EQUAL_INT(0, mean.acceleration.y);
    TEST_ASSERT_EQUAL_INT(0, mean.acceleration.z);
    TEST_ASSERT_EQUAL_INT(0, mean.rotation.x);
    TEST_ASSERT_EQUAL_INT(0, mean.rotation.y);
    TEST_ASSERT_EQUAL_INT(0, mean.rotation.z);
}

static void
test_average_rounding(void) {
    for (int sign = -1; sign <= 1; sign += 2) {
        imu_rest_t average = {0};
        average = (imu_rest_t){.acceleration = {sign, -sign, sign}, .rotation = {-sign, sign, -sign}, .n = 2};
        const input_motion_t mean = imu_rest_mean(&average);
        TEST_ASSERT_EQUAL_INT(sign, mean.acceleration.x);
        TEST_ASSERT_EQUAL_INT(-sign, mean.acceleration.y);
        TEST_ASSERT_EQUAL_INT(sign, mean.acceleration.z);
        TEST_ASSERT_EQUAL_INT(-sign, mean.rotation.x);
        TEST_ASSERT_EQUAL_INT(sign, mean.rotation.y);
        TEST_ASSERT_EQUAL_INT(-sign, mean.rotation.z);
    }
}

static void
test_recent_mean_drift_and_recovery(void) {
    static const uint32_t steps[][3] = {{TEST_DT_MS / 2, TEST_DT_MS / 2, TEST_DT_MS / 2},
                                        {2 * TEST_DT_MS, 2 * TEST_DT_MS, 2 * TEST_DT_MS},
                                        {TEST_DT_MS / 2, 2 * TEST_DT_MS, TEST_DT_MS}};
    for (int timing = 0; timing < 3; timing++) {
        imu_probe_round_t* round = calloc(1, sizeof *round);
        TEST_ASSERT_NOT_NULL(round);
        imu_probe_start(round, 0);
        input_motion_t sample = fixture();
        imu_probe_update(round, &sample, TEST_DT_MS);
        uint32_t elapsed = 0;
        int step = 0;
        bool reset = false;
        while (elapsed < IMU_REST_SETTLE_MS / 2) {
            const uint32_t dt = steps[timing][step++ % 3];
            elapsed += dt;
            sample.rotation.x =
                fixture().rotation.x + (int)(elapsed * IMU_REST_GYRO_DEPARTURE / IMU_REST_RECENT_MS * 2);
            imu_probe_update(round, &sample, dt);
            reset = reset || (elapsed > IMU_REST_RECENT_MS && (round->rest.settle_us / MATH_US_PER_MS) == 0);
        }
        TEST_ASSERT_TRUE(reset);
        sample.rotation.x += 4 * IMU_REST_GYRO_DEPARTURE;
        imu_probe_update(round, &sample, TEST_DT_MS);
        TEST_ASSERT_EQUAL_UINT32(0, (round->rest.settle_us / MATH_US_PER_MS));
        elapsed = 0;
        while (elapsed < 4 * IMU_REST_RECENT_MS) {
            const uint32_t dt = steps[timing][step++ % 3];
            elapsed += dt;
            imu_probe_update(round, &sample, dt);
        }
        TEST_ASSERT_TRUE((round->rest.settle_us / MATH_US_PER_MS) > 0);
        TEST_ASSERT_EQUAL_INT(0, round->pose);
        free(round);
    }
}

static void
test_lateral_gravity_gate(void) {
    imu_probe_round_t* round = calloc(1, sizeof *round);
    TEST_ASSERT_NOT_NULL(round);
    for (int q = 0; q < 4; q++) {
        for (int pose = IMU_PROBE_LEFT_DOWN; pose < IMU_PROBE_POSE_COUNT; pose++) {
            for (int other = 0; other < 4; other++) {
                if (other == q) {
                    continue;
                }
                imu_probe_start(round, q);
                round->pose = pose;
                input_motion_t sample = fixture();
                sample.acceleration = vec3i_scale(imu_probe_pose_direction(pose, other), INPUT_ACCEL_ONE_G);
                imu_probe_update(round, &sample, TEST_DT_MS);
                imu_probe_update(round, &sample, TEST_DT_MS);
                TEST_ASSERT_EQUAL_UINT32(0, (round->rest.settle_us / MATH_US_PER_MS));
            }
            imu_probe_start(round, q);
            round->pose = pose;
            input_motion_t sample = fixture();
            const vec3i_t direction = imu_probe_pose_direction(pose, q);
            sample.acceleration = vec3i_scale(direction, IMU_PROBE_GRAVITY_MIN - 1);
            sample.acceleration.z = INPUT_ACCEL_ONE_G;
            imu_probe_update(round, &sample, TEST_DT_MS);
            imu_probe_update(round, &sample, TEST_DT_MS);
            TEST_ASSERT_EQUAL_UINT32(0, (round->rest.settle_us / MATH_US_PER_MS));
        }
    }
    free(round);
}

static void
test_empty_fit(void) {
    imu_rest_t* poses = calloc(IMU_PROBE_POSE_COUNT, sizeof *poses);
    TEST_ASSERT_NOT_NULL(poses);
    const imu_probe_fit_t fit = imu_probe_fit(poses, 0);
    const vec3i_t values[] = {imu_probe_mg(fit.calib.offset), fit.scale_ppm, fit.gyro_mdps};
    for (size_t i = 0; i < sizeof values / sizeof values[0]; i++) {
        TEST_ASSERT_EQUAL_INT(0, values[i].x);
        TEST_ASSERT_EQUAL_INT(0, values[i].y);
        TEST_ASSERT_EQUAL_INT(0, values[i].z);
    }
    free(poses);
}

static void
test_empty_pose_ignored(void) {
    imu_rest_t* poses = calloc(IMU_PROBE_POSE_COUNT, sizeof *poses);
    TEST_ASSERT_NOT_NULL(poses);
    const input_motion_t sample = fixture();
    poses[IMU_PROBE_FACE_UP] =
        (imu_rest_t){.acceleration = {sample.acceleration.x, sample.acceleration.y, sample.acceleration.z},
                     .rotation = {sample.rotation.x, sample.rotation.y, sample.rotation.z},
                     .n = 1};
    const imu_probe_fit_t expected = imu_probe_fit(poses, 0);
    poses[IMU_PROBE_LEFT_DOWN].acceleration[0] = INPUT_ACCEL_ONE_G;
    poses[IMU_PROBE_LEFT_DOWN].rotation[0] = INPUT_ROTATION_ONE_DPS;
    poses[IMU_PROBE_LEFT_DOWN].rotation[1] = -INPUT_ROTATION_ONE_DPS;
    poses[IMU_PROBE_LEFT_DOWN].rotation[2] = 2 * INPUT_ROTATION_ONE_DPS;
    const imu_probe_fit_t fit = imu_probe_fit(poses, 0);
    TEST_ASSERT_EQUAL_MEMORY(&expected, &fit, sizeof fit);
    free(poses);
}

static void
test_synthetic_fit(void) {
    const int offset[] = {INPUT_ACCEL_ONE_G / 16, -INPUT_ACCEL_ONE_G / 8, INPUT_ACCEL_ONE_G / 32};
    const int span[] = {INPUT_ACCEL_ONE_G + (INPUT_ACCEL_ONE_G / 16), INPUT_ACCEL_ONE_G - (INPUT_ACCEL_ONE_G / 8),
                        INPUT_ACCEL_ONE_G + (INPUT_ACCEL_ONE_G / 32)};
    for (int q = 0; q < 4; q++) {
        imu_probe_round_t* round = calloc(1, sizeof *round);
        TEST_ASSERT_NOT_NULL(round);
        imu_probe_start(round, q);
        for (int pose = 0; pose < IMU_PROBE_POSE_COUNT; pose++) {
            const vec3i_t d = imu_probe_pose_direction(pose, q);
            input_motion_t sample = fixture();
            sample.acceleration =
                (vec3i_t){offset[0] + (d.x * span[0]), offset[1] + (d.y * span[1]), offset[2] + (d.z * span[2])};
            sample.rotation = (vec3i_t){(pose + 1) * INPUT_ROTATION_ONE_DPS, -2 * (pose + 1) * INPUT_ROTATION_ONE_DPS,
                                        3 * (pose + 1) * INPUT_ROTATION_ONE_DPS};
            const int n = pose + 1;
            round->poses[pose] =
                (imu_rest_t){.acceleration = {(int64_t)sample.acceleration.x * n, (int64_t)sample.acceleration.y * n,
                                              (int64_t)sample.acceleration.z * n},
                             .rotation = {(int64_t)sample.rotation.x * n, (int64_t)sample.rotation.y * n,
                                          (int64_t)sample.rotation.z * n},
                             .n = n};
        }
        round->fit = imu_probe_fit(round->poses, q);
        const vec3i_t stored = {offset[0] / 2, offset[1] / 2, offset[2] / 2};
        const vec3i_t saved = imu_probe_saved_offset(stored, &round->fit);
        TEST_ASSERT_EQUAL_INT(stored.x + offset[0], saved.x);
        TEST_ASSERT_EQUAL_INT(stored.y + offset[1], saved.y);
        TEST_ASSERT_EQUAL_INT(stored.z + offset[2], saved.z);
        const vec3i_t raw = vec3i_add(saved, (vec3i_t){0, INPUT_ACCEL_ONE_G, 0});
        const imu_calib_t calib = {.offset = saved};
        const vec3i_t corrected = imu_calib_apply(&calib, raw);
        TEST_ASSERT_EQUAL_INT(0, corrected.x);
        TEST_ASSERT_EQUAL_INT(INPUT_ACCEL_ONE_G, corrected.y);
        TEST_ASSERT_EQUAL_INT(0, corrected.z);
        TEST_ASSERT_EQUAL_INT(offset[0], round->fit.calib.offset.x);
        TEST_ASSERT_EQUAL_INT(offset[1], round->fit.calib.offset.y);
        TEST_ASSERT_EQUAL_INT(offset[2], round->fit.calib.offset.z);
        const vec3i_t offset_mg = imu_probe_mg(round->fit.calib.offset);
        const int got_off[] = {offset_mg.x, offset_mg.y, offset_mg.z};
        const int got_scale[] = {round->fit.scale_ppm.x, round->fit.scale_ppm.y, round->fit.scale_ppm.z};
        for (int axis = 0; axis < 3; axis++) {
            TEST_ASSERT_EQUAL_INT((int)mathf_round_i32((float)offset[axis] * IMU_PROBE_MILLI / INPUT_ACCEL_ONE_G),
                                  got_off[axis]);
            TEST_ASSERT_EQUAL_INT((int)mathf_round_i32((float)span[axis] * IMU_PROBE_SCALE_ONE / INPUT_ACCEL_ONE_G),
                                  got_scale[axis]);
        }
        TEST_ASSERT_EQUAL_INT((int)mathf_round_i32(13.0F * IMU_PROBE_MILLI / 3), round->fit.gyro_mdps.x);
        TEST_ASSERT_EQUAL_INT((int)mathf_round_i32(-26.0F * IMU_PROBE_MILLI / 3), round->fit.gyro_mdps.y);
        TEST_ASSERT_EQUAL_INT(13 * IMU_PROBE_MILLI, round->fit.gyro_mdps.z);
        for (int pose = 0; pose < IMU_PROBE_POSE_COUNT; pose++) {
            for (int axis = 0; axis < 3; axis++) {
                round->poses[pose].acceleration[axis] -= (int64_t)offset[axis] * round->poses[pose].n;
            }
        }
        const imu_probe_fit_t repeated = imu_probe_fit(round->poses, q);
        TEST_ASSERT_TRUE(vec3i_equal((vec3i_t){0}, repeated.calib.offset));
        TEST_ASSERT_TRUE(vec3i_equal(saved, imu_probe_saved_offset(saved, &repeated)));
        free(round);
    }
}

static void
test_fit_fractional_pose_means(void) {
    const int positive_pose_sum_bias = 8;
    const int other_pose_sum_bias = 1;
    imu_rest_t* poses = calloc(IMU_PROBE_POSE_COUNT, sizeof *poses);
    TEST_ASSERT_NOT_NULL(poses);
    for (int sign = -1; sign <= 1; sign += 2) {
        for (int pose = 0; pose < IMU_PROBE_POSE_COUNT; pose++) {
            const vec3i_t direction = imu_probe_pose_direction(pose, 0);
            const int axes[] = {direction.x, direction.y, direction.z};
            poses[pose].n = 3;
            for (int axis = 0; axis < 3; axis++) {
                poses[pose].acceleration[axis] =
                    ((int64_t)axes[axis] * INPUT_ACCEL_ONE_G * poses[pose].n)
                    + ((axes[axis] > 0 ? positive_pose_sum_bias : other_pose_sum_bias) * sign);
            }
        }
        const imu_probe_fit_t fit = imu_probe_fit(poses, 0);
        const vec3i_t expected = {2 * sign, 2 * sign, 2 * sign};
        TEST_ASSERT_EQUAL_MEMORY(&expected, &fit.calib.offset, sizeof expected);
    }
    free(poses);
}

static void
test_saved_offset_exceeds_limit(void) {
    for (int axis = 0; axis < 3; axis++) {
        for (int sign = -1; sign <= 1; sign += 2) {
            int32_t axes[3] = {0};
            axes[axis] = sign * (IMU_CALIB_OFFSET_MAX - 1);
            const vec3i_t stored = {axes[0], axes[1], axes[2]};
            axes[axis] = sign * 2;
            const imu_probe_fit_t fit = {.calib = {.offset = {axes[0], axes[1], axes[2]}}};
            TEST_ASSERT_TRUE(imu_calib_valid(stored, IMU_CALIB_OFFSET_MAX));
            TEST_ASSERT_TRUE(imu_calib_valid(fit.calib.offset, IMU_CALIB_OFFSET_MAX));
            TEST_ASSERT_FALSE(imu_calib_valid(imu_probe_saved_offset(stored, &fit), IMU_CALIB_OFFSET_MAX));
        }
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
            for (int ms = 0; ms < IMU_REST_SETTLE_MS + IMU_REST_WINDOW_MS; ms += TEST_DT_MS) {
                TEST_ASSERT_FALSE(imu_probe_update(round, &sample, TEST_DT_MS));
            }
            TEST_ASSERT_TRUE(imu_probe_update(round, &sample, TEST_DT_MS));
            TEST_ASSERT_FALSE(round->rest.primed);
            if (round->step == IMU_PROBE_POSES) {
                TEST_ASSERT_FALSE(imu_probe_update(round, &sample, TEST_DT_MS));
                TEST_ASSERT_TRUE(round->rest.primed);
                TEST_ASSERT_EQUAL_UINT32(0, (round->rest.settle_us / MATH_US_PER_MS));
                imu_probe_update(round, NULL, TEST_DT_MS);
            }
        }
        TEST_ASSERT_EQUAL_INT(IMU_PROBE_UPRIGHT, round->step);
        TEST_ASSERT_EQUAL_INT(IMU_PROBE_POSE_COUNT, round->pose);
        TEST_ASSERT_EQUAL_INT(IMU_PROBE_SCALE_ONE, round->fit.scale_ppm.x);
        TEST_ASSERT_EQUAL_INT(0, imu_probe_mg(round->fit.calib.offset).x);
        const input_motion_t sample = fixture();
        TEST_ASSERT_EQUAL_INT(IMU_PROBE_SCALE_ONE, round->fit.scale_ppm.y);
        TEST_ASSERT_EQUAL_INT(IMU_PROBE_SCALE_ONE, round->fit.scale_ppm.z);
        TEST_ASSERT_EQUAL_INT(0, imu_probe_mg(round->fit.calib.offset).y);
        TEST_ASSERT_EQUAL_INT(0, imu_probe_mg(round->fit.calib.offset).z);
        TEST_ASSERT_EQUAL_INT(10 * IMU_PROBE_MILLI, round->fit.gyro_mdps.x);
        TEST_ASSERT_EQUAL_INT(-10 * IMU_PROBE_MILLI, round->fit.gyro_mdps.y);
        TEST_ASSERT_EQUAL_INT(0, round->fit.gyro_mdps.z);
        TEST_ASSERT_FALSE(imu_probe_update(round, &sample, TEST_DT_MS));
    }
    free(round);
}

static void
test_zero_acceleration_cannot_arm_upright(void) {
    imu_probe_round_t* round = calloc(1, sizeof *round);
    TEST_ASSERT_NOT_NULL(round);
    imu_probe_start(round, 0);
    round->step = IMU_PROBE_UPRIGHT;
    round->pose = IMU_PROBE_POSE_COUNT;
    const input_motion_t zero = {0};
    for (int ms = 0; ms <= IMU_REST_SETTLE_MS; ms += TEST_DT_MS) {
        TEST_ASSERT_FALSE(imu_probe_turn_update(round, &zero, (vec3f_t){0}, TEST_DT_MS));
        TEST_ASSERT_EQUAL_INT(IMU_PROBE_UPRIGHT, round->step);
        TEST_ASSERT_EQUAL_UINT32(0, (round->rest.settle_us / MATH_US_PER_MS));
    }
    free(round);
}

void
run_imu_probe_suite(void) {
    RUN_TEST(test_zero_acceleration_cannot_arm_upright);
    RUN_TEST(test_settle_and_window);
    RUN_TEST(test_mapping_and_pose_names);
    RUN_TEST(test_departure_boundaries);
    RUN_TEST(test_gravity_gate_and_wrong_pose);
    RUN_TEST(test_movement_resets_window);
    RUN_TEST(test_gap_boundary);
    RUN_TEST(test_zero_dt);
    RUN_TEST(test_missing_reading);
    RUN_TEST(test_restart);
    RUN_TEST(test_average_empty);
    RUN_TEST(test_average_rounding);
    RUN_TEST(test_recent_mean_drift_and_recovery);
    RUN_TEST(test_lateral_gravity_gate);
    RUN_TEST(test_empty_fit);
    RUN_TEST(test_empty_pose_ignored);
    RUN_TEST(test_synthetic_fit);
    RUN_TEST(test_complete_round);
    RUN_TEST(test_fit_fractional_pose_means);
    RUN_TEST(test_saved_offset_exceeds_limit);
}

SUITE_REGISTER(run_imu_probe_suite);
