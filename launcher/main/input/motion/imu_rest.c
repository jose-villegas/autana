#include "input/motion/imu_rest.h"

#include <math.h>
#include "math/scalar/mathf_round.h"

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

static void
window_step(imu_rest_t* rest, const input_motion_t* sample, bool measuring) {
    if (!measuring) {
        rest->n = 0;
        for (int axis = 0; axis < 3; axis++) {
            rest->acceleration[axis] = rest->rotation[axis] = 0;
        }
        return;
    }
    const int32_t a[] = {sample->acceleration.x, sample->acceleration.y, sample->acceleration.z};
    const int32_t r[] = {sample->rotation.x, sample->rotation.y, sample->rotation.z};
    for (int axis = 0; axis < 3; axis++) {
        rest->acceleration[axis] += a[axis];
        rest->rotation[axis] += r[axis];
    }
    rest->n++;
}

imu_rest_stage_t
imu_rest_step(imu_rest_t* rest, const input_motion_t* sample, uint32_t dt_us, bool gate) {
    if (dt_us == 0) {
        return IMU_REST_MOVING;
    }
    if (!rest->primed) {
        *rest = (imu_rest_t){0};
    }
    bool stable = still(rest, sample, dt_us) && gate;
    if (stable && rest->settle_us != 0) {
        for (int axis = 0; axis < 3; axis++) {
            stable = stable && fabsf(rest->recent_accel[axis] - rest->anchor[axis]) <= IMU_REST_DRIFT;
        }
    }
    window_step(rest, sample, stable && rest->settle_us >= IMU_REST_SETTLE_MS * MATH_US_PER_MS);
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
    rest->primed = false;
    rest->settle_us = rest->window_us = 0;
    return IMU_REST_DONE;
}

input_motion_t
imu_rest_mean(const imu_rest_t* rest) {
    if (rest->n == 0) {
        return (input_motion_t){0};
    }
    int32_t a[3];
    int32_t r[3];
    for (int axis = 0; axis < 3; axis++) {
        a[axis] = mathf_round_i32((float)rest->acceleration[axis] / (float)rest->n);
        r[axis] = mathf_round_i32((float)rest->rotation[axis] / (float)rest->n);
    }
    return (input_motion_t){.acceleration = {a[0], a[1], a[2]}, .rotation = {r[0], r[1], r[2]}};
}
