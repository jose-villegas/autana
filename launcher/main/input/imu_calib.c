#include "input/imu_calib.h"

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
imu_calib_from_saved(const bool found[3], const int32_t saved[3]) {
    for (int axis = 0; axis < 3; axis++) {
        if (!found[axis] || saved[axis] < -IMU_CALIB_OFFSET_MAX || saved[axis] > IMU_CALIB_OFFSET_MAX) {
            return (imu_calib_t){0};
        }
    }
    return (imu_calib_t){.offset = {saved[0], saved[1], saved[2]}};
}
