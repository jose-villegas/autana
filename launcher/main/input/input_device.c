/*
 * input_device: input.h's calls, over the touch, button and motion drivers.
 * Device only; the host builds input_t without it.
 */

#include "input/input_shell.h"

#include "esp_log.h"

#include "input/buttons.h"
#include "input/imu.h"
#include "input/imu_rotation.h"
#include "input/touch.h"

static const char TAG[] = "input";

void
input_start(void) {
    touch_start();
    buttons_start();
    if (!imu_init()) {
        ESP_LOGW(TAG, "No IMU - display orientation stays upright");
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
    out->gravity = imu_gravity_screen(&sample);
    out->gravity_z = sample.az;
    out->turn_rate = imu_rotation_level(&sample);
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
