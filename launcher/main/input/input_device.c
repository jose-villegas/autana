/*
 * input_device: input.h's calls, over the touch, button and motion drivers.
 * Device only; the host builds input_t without it.
 */

#include "input/input_shell.h"

#include <string.h>

#include "esp_log.h"

#include "core/timing.h"
#include "input/buttons/buttons.h"
#include "input/motion/imu.h"
#include "input/motion/imu_calib.h"
#include "input/motion/motion_reader.h"
#include "input/touch/touch.h"
#include "math/linear/vec_convert.h"
#include "services/settings.h"

static const char TAG[] = "input";

#define IMU_SETTINGS_SPACE       "imu"
#define IMU_SETTINGS_ACCEL_X     "accel_x"
#define IMU_SETTINGS_ACCEL_Y     "accel_y"
#define IMU_SETTINGS_ACCEL_Z     "accel_z"
#define IMU_SETTINGS_ACCEL_VALID "accel_valid"

static const char* const ACCEL_KEYS[] = {IMU_SETTINGS_ACCEL_X, IMU_SETTINGS_ACCEL_Y, IMU_SETTINGS_ACCEL_Z};
static imu_calib_t accel_calib;

_Static_assert(INPUT_ACCEL_ONE_G == IMU_COUNTS_PER_G, "input_read_motion() passes sensor counts through");

_Static_assert(INPUT_ROTATION_ONE_DPS == IMU_COUNTS_PER_DPS, "rotation passes sensor counts through");
_Static_assert(INPUT_MOTION_SAMPLE_US == IMU_SAMPLE_US, "batch timing matches the sensor ODR");
_Static_assert(INPUT_MOTION_SAMPLES == IMU_FIFO_SAMPLES, "batch holds the whole FIFO");
_Static_assert(sizeof(input_motion_t) >= sizeof(imu_sample_t), "raw samples expand in place");

void
input_start(void) {
    motion_reader_reset();
    int32_t saved[3] = {0};
    bool found[3];
    for (int axis = 0; axis < 3; axis++) {
        found[axis] = settings_read_i32(IMU_SETTINGS_SPACE, ACCEL_KEYS[axis], &saved[axis]);
    }
    int32_t marker = 0;
    const bool committed = settings_read_i32(IMU_SETTINGS_SPACE, IMU_SETTINGS_ACCEL_VALID, &marker) && marker == 1;
    accel_calib = imu_calib_from_saved(committed, found, saved);
    touch_start();
    buttons_start();
    if (!imu_init()) {
        ESP_LOGW(TAG, "No IMU - motion reads report nothing");
    }
    imu_start();
}

vec3i_t
input_accel_offset(void) {
    return accel_calib.offset;
}

bool
input_set_accel_offset(vec3i_t offset) {
    const int32_t saved[] = {offset.x, offset.y, offset.z};
    if (!imu_calib_valid(offset)) {
        return false;
    }
    if (!settings_write_i32(IMU_SETTINGS_SPACE, IMU_SETTINGS_ACCEL_VALID, 0)) {
        return false;
    }
    for (int axis = 0; axis < 3; axis++) {
        if (!settings_write_i32(IMU_SETTINGS_SPACE, ACCEL_KEYS[axis], saved[axis])) {
            return false;
        }
    }
    if (!settings_write_i32(IMU_SETTINGS_SPACE, IMU_SETTINGS_ACCEL_VALID, 1)) {
        return false;
    }
    accel_calib = (imu_calib_t){.offset = offset};
    return true;
}

void
input_poll(input_t* out) {
    motion_reader_frame(timing_now_us());
    touch_read(out);
    buttons_read(&out->boot, &out->power);
}

static input_motion_t
motion_from_sample(const imu_sample_t* sample) {
    return (input_motion_t){.acceleration =
                                imu_calib_apply(&accel_calib, imu_axes_screen(sample->ax, sample->ay, sample->az)),
                            .turn_speed = imu_turn_speed(sample),
                            .rotation = imu_axes_screen(sample->gx, sample->gy, sample->gz)};
}

bool
input_read_motion(input_motion_t* out) {
    imu_sample_t sample;
    if (!imu_read(&sample)) {
        return false;
    }
    *out = motion_from_sample(&sample);
    return true;
}

bool
input_read_motion_batch(input_motion_batch_t* out) {
    out->count = 0;
    out->gap = false;
    const int count = imu_take_fifo((imu_sample_t*)out->samples, out->lost_before, &out->gap);
    if (count < 0) {
        return false;
    }
    /* Expansion runs backwards so a converted sample cannot overwrite unread raw axes. */
    for (int i = count - 1; i >= 0; i--) {
        imu_sample_t raw;
        memcpy(&raw, (const unsigned char*)out->samples + ((size_t)i * sizeof raw), sizeof raw);
        out->samples[i] = motion_from_sample(&raw);
    }
    out->count = count;
    return true;
}

bool
input_read_acceleration(vec3f_t* acceleration) {
    input_motion_t motion;
    if (!input_read_motion(&motion)) {
        return false;
    }
    *acceleration = vec3f_scale(vec3f_from_vec3i(motion.acceleration), 1.0F / INPUT_ACCEL_ONE_G);
    return true;
}
