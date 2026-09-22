/*
 * console_inject - TOUCH and IMU, letting a host script stand in for a
 * touch controller and IMU while a development build is driven from the
 * console.
 *
 * Neither sets a flag for the frame loop the way SCREENSHOT/RUNSUITE do:
 * what each writes is a LEVEL the polling task samples at its own rate,
 * which is the contract touch_inject()/imu_inject() are built for - the
 * state machine below touch derives the edges from that level. Where a
 * real controller answers, injection takes precedence only until released.
 */
#include "console/console_inject_parse.h"
#include "console/console_verbs.h"

#include "input/imu.h"
#include "input/touch.h"

#include "esp_log.h"

#include <string.h>

static const char* TAG = "console";

static void
console_verb_touch(const char* args, console_reply_fn reply) {
    (void)reply;
    bool down = false;
    int x = 0, y = 0;
    if (console_touch_parse(args, &down, &x, &y)) {
        touch_inject(down, x, y);
    } else {
        ESP_LOGW(TAG, "TOUCH wants <down|up> <x> <y>: '%s'", args);
    }
}

static void
console_verb_imu(const char* args, console_reply_fn reply) {
    (void)reply;
    int ax = 0, ay = 0, az = 0;
    if (strcmp(args, "release") == 0) {
        imu_inject_release();
    } else if (console_imu_parse(args, &ax, &ay, &az)) {
        imu_inject(&(imu_sample_t){.ax = (int16_t)ax, .ay = (int16_t)ay, .az = (int16_t)az});
    } else {
        ESP_LOGW(TAG, "IMU wants <ax> <ay> <az> or release: '%s'", args);
    }
}

CONSOLE_VERB(touch, 14, console_verb_touch)
CONSOLE_VERB(imu, 20, console_verb_imu)
