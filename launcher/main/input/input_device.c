/*
 * input_device: input.h's calls, over the touch, button and motion drivers.
 * Device only; the host builds input_t without it.
 */

#include "input/input_shell.h"

#include "esp_log.h"

#include "input/buttons.h"
#include "input/imu.h"
#include "input/touch.h"

static const char TAG[] = "input";

_Static_assert(INPUT_GRAVITY_PER_G == IMU_COUNTS_PER_G, "input_read_motion() passes sensor counts through");

void
input_start(void) {
    touch_start();
    buttons_start();
    if (!imu_init()) {
        ESP_LOGW(TAG, "No IMU - motion reads report nothing");
    }
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
    *out = (input_motion_t){imu_gravity_screen(&sample), sample.az, imu_rotation_level(&sample)};
    return true;
}

bool
input_read_gravity(vec2i_t* gravity) {
    input_motion_t motion;
    if (!input_read_motion(&motion)) {
        return false;
    }
    *gravity = motion.gravity;
    return true;
}
