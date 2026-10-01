/*
 * input_device: input.h's calls, over the touch, button and motion drivers.
 * Device only; the host builds input_t without it.
 */

#include "input/input.h"

#include "esp_log.h"

#include "input/buttons.h"
#include "input/imu.h"
#include "input/touch.h"

static const char* TAG = "input";

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
input_read_motion(imu_sample_t* out) {
    return imu_ready() && imu_read(out);
}

#if CONFIG_LAUNCHER_DEVELOPMENT
void
input_take_touch_sample_counts(uint32_t* points, uint32_t* moved) {
    touch_take_sample_counts(points, moved);
}

const char*
input_take_gesture_completion_name(void) {
    touch_gesture_completion_t completion;
    if (!touch_gesture_take_completion(&completion)) {
        return NULL;
    }
    return completion == TOUCH_GESTURE_TAP ? "TAP" : completion == TOUCH_GESTURE_PRESS ? "PRESS" : "DRAG";
}
#endif
