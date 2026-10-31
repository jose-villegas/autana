#include "input/motion/imu_calib.h"

#include "math/scalar/mathf_round.h"

imu_calib_fit_t
imu_calib_fit(vec3i_t up, vec3i_t down) {
    const int32_t positive[] = {up.x, up.y, up.z};
    const int32_t negative[] = {down.x, down.y, down.z};
    int32_t offset[3];
    int32_t scale[3];
    for (int axis = 0; axis < 3; axis++) {
        offset[axis] = mathf_round_i32(((float)positive[axis] + (float)negative[axis]) / 2);
        scale[axis] = mathf_round_i32(((float)positive[axis] - (float)negative[axis]) * IMU_CALIB_SCALE_ONE
                                      / (2 * INPUT_ACCEL_ONE_G));
    }
    return (imu_calib_fit_t){.calib = {.offset = {offset[0], offset[1], offset[2]}},
                             .scale_ppm = {scale[0], scale[1], scale[2]}};
}

vec3i_t
imu_calib_apply(const imu_calib_t* calib, vec3i_t raw) {
    return vec3i_sub(raw, calib->offset);
}

bool
imu_calib_from_saved(bool committed, const bool found[3], const int32_t saved[3], int32_t max, imu_calib_t* out) {
    *out = (imu_calib_t){0};
    if (!committed) {
        return false;
    }
    for (int axis = 0; axis < 3; axis++) {
        if (!found[axis]) {
            return false;
        }
    }
    const vec3i_t offset = {saved[0], saved[1], saved[2]};
    if (!imu_calib_valid(offset, max)) {
        return false;
    }
    out->offset = offset;
    return true;
}

bool
imu_calib_valid(vec3i_t offset, int32_t max) {
    const int32_t axes[] = {offset.x, offset.y, offset.z};
    for (int axis = 0; axis < 3; axis++) {
        if (axes[axis] < -max || axes[axis] > max) {
            return false;
        }
    }
    return true;
}

imu_calib_gyro_t
imu_calib_gyro_from_saved(bool committed, const bool found[3], const int32_t saved[3]) {
    imu_calib_gyro_t gyro = {0};
    gyro.valid = imu_calib_from_saved(committed, found, saved, IMU_CALIB_GYRO_MAX, &gyro.live);
    gyro.storage_valid = gyro.valid;
    gyro.saved = gyro.live.offset;
    return gyro;
}

imu_calib_gyro_step_t
imu_calib_gyro_learn(imu_calib_gyro_t* gyro, vec3i_t residual, int64_t now_us) {
    if (!imu_calib_valid(residual, gyro->valid ? IMU_CALIB_GYRO_RESIDUAL_MAX : IMU_CALIB_GYRO_MAX)) {
        return IMU_CALIB_GYRO_REJECTED;
    }
    const vec3i_t offset = vec3i_add(gyro->live.offset, residual);
    if (!imu_calib_valid(offset, IMU_CALIB_GYRO_MAX)) {
        return IMU_CALIB_GYRO_REJECTED;
    }
    gyro->live.offset = offset;
    gyro->valid = true;
    const bool interval = now_us >= gyro->next_save_us;
    const bool changed = !imu_calib_valid(vec3i_sub(gyro->live.offset, gyro->saved), IMU_CALIB_GYRO_SAVE_DELTA);
    return interval && (!gyro->storage_valid || changed) ? IMU_CALIB_GYRO_SAVE_DUE : IMU_CALIB_GYRO_APPLIED;
}

input_motion_t
imu_calib_gyro_correct(const imu_calib_gyro_t* gyro, input_motion_t sample) {
    sample.rotation = imu_calib_apply(&gyro->live, sample.rotation);
    sample.turn_speed = imu_turn_speed(sample.rotation);
    return sample;
}

void
imu_calib_gyro_saved(imu_calib_gyro_t* gyro, bool ok, int64_t now_us) {
    if (ok) {
        gyro->saved = gyro->live.offset;
    }
    gyro->storage_valid = ok;
    gyro->next_save_us = now_us + IMU_CALIB_GYRO_SAVE_INTERVAL_US;
}
