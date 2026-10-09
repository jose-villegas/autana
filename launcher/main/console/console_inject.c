/*
 * console_inject (TOUCH, IMU, TAP, PRESS, DRAG and BUTTON): a host script
 * standing in for the touch controller, the IMU and the board buttons on a
 * development build.
 *
 * None sets a flag for the frame loop the way SCREENSHOT/RUNSUITE do. A
 * touch or IMU sample is a LEVEL the polling task reads at its own rate
 * until something releases it (the contract touch_inject()/imu_inject()
 * are built for); a gesture is that level played out against the poller's
 * clock until its duration runs out, and a button edge is consumed by the
 * next read. Whatever is injected outranks the hardware while it lasts.
 */
#include "console/console_inject_parse.h"
#include "console/console_verbs.h"

#include "input/buttons.h"
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
        touch_inject(down, (vec2i_t){x, y});
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

static void
start_gesture(const char* args, bool (*parse)(const char* args, console_touch_gesture_t* out),
              touch_gesture_completion_t kind, const char* usage) {
    console_touch_gesture_t gesture;
    if (parse(args, &gesture)) {
        touch_gesture_start(gesture.x0, gesture.y0, gesture.x1, gesture.y1, gesture.ms, kind);
    } else {
        ESP_LOGW(TAG, "%s: '%s'", usage, args);
    }
}

static void
console_verb_tap(const char* args, console_reply_fn reply) {
    (void)reply;
    start_gesture(args, console_tap_parse, TOUCH_GESTURE_TAP, "TAP wants <x> <y>");
}

static void
console_verb_press(const char* args, console_reply_fn reply) {
    (void)reply;
    start_gesture(args, console_press_parse, TOUCH_GESTURE_PRESS, "PRESS wants <x> <y> [ms]");
}

static void
console_verb_drag(const char* args, console_reply_fn reply) {
    (void)reply;
    start_gesture(args, console_drag_parse, TOUCH_GESTURE_DRAG, "DRAG wants <x0> <y0> <x1> <y1> <ms>");
}

static void
console_verb_button(const char* args, console_reply_fn reply) {
    console_button_t button;
    bool held;
    if (console_button_parse(args, &button, &held)) {
        buttons_inject(button == CONSOLE_BUTTON_BOOT ? BUTTONS_INJECT_BOOT : BUTTONS_INJECT_POWER, held);
        reply("BUTTON_OK");
    } else {
        ESP_LOGW(TAG, "BUTTON wants <boot|power> [short|long]: '%s'", args);
        reply("BUTTON_ERR");
    }
}

CONSOLE_VERB(touch, 14, console_verb_touch)
CONSOLE_VERB(imu, 20, console_verb_imu)
CONSOLE_VERB(tap, 13, console_verb_tap)
CONSOLE_VERB(press, 13, console_verb_press)
CONSOLE_VERB(drag, 23, console_verb_drag)
CONSOLE_VERB(button, 11, console_verb_button)
