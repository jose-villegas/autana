#include "input/motion/imu_rest.h"

#include <math.h>
#include "input/motion/gravity.h"
#include "math/linear/vec_convert.h"
#include "math/scalar/mathf_round.h"

void
imu_rest_reset(imu_rest_t* rest) {
    *rest = (imu_rest_t){0};
}

bool
imu_rest_still(imu_rest_t* rest, const input_motion_t* sample, uint32_t dt_us) {
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

bool
imu_rest_update(imu_rest_t* rest, const input_motion_t* sample, uint32_t dt_us, vec3i_t* residual) {
    if (dt_us == 0) {
        return false;
    }
    bool stable = imu_rest_still(rest, sample, dt_us);
    if (sample != NULL) {
        const vec3f_t a = vec3f_scale(vec3f_from_vec3i(sample->acceleration), 1.0F / INPUT_ACCEL_ONE_G);
        stable = stable && fabsf(sqrtf(vec3f_dot(a, a)) - 1.0F) <= GRAVITY_TRUST_FULL_G;
    }
    if (!stable) {
        rest->settle_us = rest->window_us = rest->samples = 0;
        for (int axis = 0; axis < 3; axis++) {
            rest->rotation[axis] = 0;
        }
        return false;
    }
    if (rest->settle_us < IMU_REST_SETTLE_MS * MATH_US_PER_MS) {
        rest->settle_us += dt_us;
        return false;
    }
    const int32_t r[] = {sample->rotation.x, sample->rotation.y, sample->rotation.z};
    for (int axis = 0; axis < 3; axis++) {
        rest->rotation[axis] += r[axis];
    }
    rest->samples++;
    rest->window_us += dt_us;
    if (rest->window_us < IMU_REST_WINDOW_MS * MATH_US_PER_MS) {
        return false;
    }
    *residual = (vec3i_t){mathf_round_i32((float)rest->rotation[0] / rest->samples),
                          mathf_round_i32((float)rest->rotation[1] / rest->samples),
                          mathf_round_i32((float)rest->rotation[2] / rest->samples)};
    imu_rest_reset(rest);
    return true;
}
