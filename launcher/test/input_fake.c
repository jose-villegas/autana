#include "input_fake.h"

#include <stddef.h>
#include "core/timing.h"
#include "input/input_shell.h"
#include "input/motion/imu.h"
#include "input/motion/imu_calib.h"
#include "input/motion/motion_reader.h"

static const input_motion_batch_t* motion_batch;
static input_motion_t motion;
static bool has_motion;
static bool sensor_present;
static uint32_t motion_sequence;
static imu_calib_gyro_t gyro_calib;

void
input_fake_motion_present(bool present) {
    sensor_present = present;
}

bool
imu_ready(void) {
    return sensor_present;
}

void
input_fake_motion(const input_motion_t* next) {
    has_motion = next != NULL;
    if (has_motion) {
        motion = *next;
    }
    motion_sequence++;
    sensor_present = next != NULL;
}

bool
input_read_motion(input_motion_t* out) {
    if (!has_motion) {
        return false;
    }
    *out = imu_calib_gyro_correct(&gyro_calib, motion);
    return true;
}

bool
input_read_motion_sequence(input_motion_t* out, uint32_t* sequence) {
    if (!input_read_motion(out)) {
        return false;
    }
    *sequence = motion_sequence;
    return true;
}

void
input_fake_motion_batch(const input_motion_batch_t* next) {
    motion_batch = next;
}

void
input_fake_reset(void) {
    input_fake_motion(NULL);
    input_fake_motion_batch(NULL);
    gyro_calib = (imu_calib_gyro_t){0};
}

bool
input_read_motion_batch(input_motion_batch_t* out) {
    if (motion_batch == NULL) {
        return false;
    }
    *out = *motion_batch;
    for (int i = 0; i < out->count; i++) {
        out->samples[i] = imu_calib_gyro_correct(&gyro_calib, out->samples[i]);
    }
    return true;
}

vec3i_t
input_gyro_offset(void) {
    return gyro_calib.live.offset;
}

bool
input_learn_gyro_offset(vec3i_t residual) {
    const int64_t now_us = timing_now_us();
    const imu_calib_gyro_step_t step = imu_calib_gyro_learn(&gyro_calib, residual, now_us);
    if (step == IMU_CALIB_GYRO_SAVE_DUE) {
        imu_calib_gyro_saved(&gyro_calib, true, now_us);
    }
    return step != IMU_CALIB_GYRO_REJECTED;
}

void
input_fake_stored_gyro(vec3i_t offset) {
    const bool found[] = {true, true, true};
    const int32_t values[] = {offset.x, offset.y, offset.z};
    gyro_calib = imu_calib_gyro_from_saved(true, found, values);
}
