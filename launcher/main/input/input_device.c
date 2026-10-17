/*
 * input_device: input.h's calls, over the touch, button and motion drivers.
 * Device only; the host builds input_t without it.
 */

#include "input/input_shell.h"

#include "esp_log.h"

#include "input/buttons.h"
#include "input/imu.h"
#include "input/imu_calib.h"
#include "input/touch.h"
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

void
input_start(void) {
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
    touch_read(out);
    buttons_read(&out->boot, &out->power);
}

bool
input_read_motion(input_motion_t* out) {
    imu_sample_t sample;
    if (!imu_read(&sample)) {
        return false;
    }
    *out = (input_motion_t){.acceleration =
                                imu_calib_apply(&accel_calib, imu_axes_screen(sample.ax, sample.ay, sample.az)),
                            .turn_speed = imu_turn_speed(&sample),
                            .rotation = imu_axes_screen(sample.gx, sample.gy, sample.gz)};
    return true;
}

bool
input_read_gravity(vec2i_t* gravity) {
    input_motion_t motion;
    if (!input_read_motion(&motion)) {
        return false;
    }
    *gravity = vec3i_xy(motion.acceleration);
    return true;
}
