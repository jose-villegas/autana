#include "input/motion/imu_rest.h"

#include <math.h>

void
imu_rest_reset(imu_rest_t* rest) {
    *rest = (imu_rest_t){0};
}

static bool
still(imu_rest_t* rest, const input_motion_t* sample, uint32_t dt_us) {
    if (sample == NULL || dt_us > IMU_REST_MAX_GAP_MS * MATH_US_PER_MS) {
        imu_rest_reset(rest);
        return false;
    }
    const int32_t a[] = {sample->acceleration.x, sample->acceleration.y, sample->acceleration.z};
    const int32_t r[] = {sample->rotation.x, sample->rotation.y, sample->rotation.z};
    const float weight = (float)dt_us / (IMU_REST_RECENT_MS * MATH_US_PER_MS + (float)dt_us);
    bool stable = rest->primed;
    for (int axis = 0; axis < 3; axis++) {
        if (!rest->primed) {
            rest->recent_accel[axis] = (float)a[axis];
            rest->recent_gyro[axis] = (float)r[axis];
        }
        stable = stable && fabsf((float)a[axis] - rest->recent_accel[axis]) < IMU_REST_ACCEL_DEPARTURE
                 && fabsf((float)r[axis] - rest->recent_gyro[axis]) < IMU_REST_GYRO_DEPARTURE;
        rest->recent_accel[axis] += weight * ((float)a[axis] - rest->recent_accel[axis]);
        rest->recent_gyro[axis] += weight * ((float)r[axis] - rest->recent_gyro[axis]);
    }
    rest->primed = true;
    return stable;
}

imu_rest_stage_t
imu_rest_step(imu_rest_t* rest, const input_motion_t* sample, uint32_t dt_us, bool gate) {
    if (dt_us == 0) {
        return IMU_REST_MOVING;
    }
    bool stable = still(rest, sample, dt_us) && gate;
    if (stable && rest->settle_us != 0) {
        for (int axis = 0; axis < 3; axis++) {
            stable = stable && fabsf(rest->recent_accel[axis] - rest->anchor[axis]) <= IMU_REST_DRIFT;
        }
    }
    if (!stable) {
        rest->settle_us = rest->window_us = 0;
        return IMU_REST_MOVING;
    }
    if (rest->settle_us == 0) {
        for (int axis = 0; axis < 3; axis++) {
            rest->anchor[axis] = rest->recent_accel[axis];
        }
    }
    if (rest->settle_us < IMU_REST_SETTLE_MS * MATH_US_PER_MS) {
        rest->settle_us += dt_us;
        return IMU_REST_SETTLING;
    }
    rest->window_us += dt_us;
    if (rest->window_us < IMU_REST_WINDOW_MS * MATH_US_PER_MS) {
        return IMU_REST_MEASURING;
    }
    imu_rest_reset(rest);
    return IMU_REST_DONE;
}
