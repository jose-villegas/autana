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

imu_calib_t
imu_calib_from_saved(bool committed, const bool found[3], const int32_t saved[3], int32_t max) {
    if (!committed) {
        return (imu_calib_t){0};
    }
    for (int axis = 0; axis < 3; axis++) {
        if (!found[axis]) {
            return (imu_calib_t){0};
        }
    }
    const vec3i_t offset = {saved[0], saved[1], saved[2]};
    return imu_calib_valid(offset, max) ? (imu_calib_t){.offset = offset} : (imu_calib_t){0};
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

bool
imu_calib_learn_gyro(imu_calib_gyro_t* gyro, vec3i_t residual) {
    if (!imu_calib_valid(residual, gyro->valid ? IMU_CALIB_GYRO_RESIDUAL_MAX : IMU_CALIB_GYRO_MAX)) {
        return false;
    }
    const vec3i_t offset = vec3i_add(gyro->live.offset, residual);
    if (!imu_calib_valid(offset, IMU_CALIB_GYRO_MAX)) {
        return false;
    }
    gyro->live.offset = offset;
    gyro->valid = true;
    return true;
}

bool
imu_calib_gyro_save_due(const imu_calib_gyro_t* gyro, int64_t now_us) {
    return gyro->valid
           && (!gyro->has_saved
               || (now_us >= gyro->saved_us && now_us - gyro->saved_us >= IMU_CALIB_GYRO_SAVE_INTERVAL_US))
           && !imu_calib_valid(vec3i_sub(gyro->live.offset, gyro->saved), IMU_CALIB_GYRO_SAVE_DELTA);
}

void
imu_calib_gyro_saved(imu_calib_gyro_t* gyro, int64_t now_us) {
    gyro->saved = gyro->live.offset;
    gyro->saved_us = now_us;
    gyro->has_saved = true;
}
