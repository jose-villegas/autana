/*
 * console_inject_parse: reading the args of the verbs that stand in for
 * touch, the IMU and the board buttons (see console_inject.c). Pure and
 * portable: no verb dispatch, no controller, just token reads and bounds.
 */
#pragma once

#include "console/console_number.h"
#include "display/display_judge.h"
#include "display/viewport.h"
#include "input/motion/imu_sample.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CONSOLE_IMU_REPLY_FORMAT        "IMU_OK quarter=%d quarters=%d sample_ms=%d commit_ms=%d"
#define CONSOLE_IMU_REPLY_INTEGER_COUNT 4
#define CONSOLE_IMU_REPLY_SIZE                                                                                         \
    (sizeof CONSOLE_IMU_REPLY_FORMAT + (CONSOLE_IMU_REPLY_INTEGER_COUNT * sizeof "-2147483648"))

static inline bool
console_imu_reply(char* line, size_t size, int quarter) {
    const int written = snprintf(line, size, CONSOLE_IMU_REPLY_FORMAT, quarter, DISPLAY_QUARTER_COUNT,
                                 DISPLAY_SAMPLE_MS, DISPLAY_COMMIT_MS);
    return written >= 0 && (size_t)written < size;
}

/* Reads `<down|up> <x> <y>` into its three parts, in panel coordinates.
 * Returns false and writes nothing for a malformed line: a caller acts on a
 * whole sample or on none of it, never on half of one. Coordinates are not
 * range-checked here: the panel's size is not this header's to know, and a
 * sample past the edge is a caller's question. */
static inline bool
console_touch_parse(const char* args, bool* down, vec2i_t* point) {
    const bool is_down = console_word(&args, "down");
    int32_t px, py;

    if ((!is_down && !console_word(&args, "up")) || !console_i32(&args, &px) || !console_i32(&args, &py)
        || *args != '\0') {
        return false;
    }
    *down = is_down;
    *point = (vec2i_t){px, py};
    return true;
}

/* Reads `<ax> <ay> <az>`, the accelerometer in raw counts. False, and
 * nothing written, for a malformed line or a count the sensor's 16 bits
 * could not give. */
static inline bool
console_imu_parse(const char* args, int* ax, int* ay, int* az) {
    int32_t a, b, c;

    if (!console_i32(&args, &a) || !console_i32(&args, &b) || !console_i32(&args, &c) || *args != '\0' || a < INT16_MIN
        || a > INT16_MAX || b < INT16_MIN || b > INT16_MAX || c < INT16_MIN || c > INT16_MAX) {
        return false;
    }
    *ax = a;
    *ay = b;
    *az = c;
    return true;
}

/* Transpose the sensor's orthogonal axis mapping to construct a raw sample
 * whose gravity follows the viewport's down axis. */
static inline bool
console_imu_quarter_parse(const char* args, imu_sample_t* sample, int* parsed_quarter) {
    int32_t quarter;
    if (!console_word(&args, "quarter") || !console_i32(&args, &quarter) || *args != '\0' || quarter < 0
        || quarter >= DISPLAY_QUARTER_COUNT) {
        return false;
    }
    *parsed_quarter = quarter;
    const viewport_quarter_axes_t axes = viewport_quarter_axes(quarter);
    const vec2i_t down = {axes.x_down, axes.y_down};
    const vec3i_t raw_x = imu_axes_screen(IMU_COUNTS_PER_G, 0, 0);
    const vec3i_t raw_y = imu_axes_screen(0, IMU_COUNTS_PER_G, 0);
    const vec3i_t raw_z = imu_axes_screen(0, 0, IMU_COUNTS_PER_G);
    *sample = (imu_sample_t){
        .ax = (int16_t)vec2i_dot(down, vec3i_xy(raw_x)),
        .ay = (int16_t)vec2i_dot(down, vec3i_xy(raw_y)),
        .az = (int16_t)vec2i_dot(down, vec3i_xy(raw_z)),
    };
    return true;
}

typedef struct {
    vec2i_t from;
    vec2i_t to;
    uint32_t ms;
} console_touch_gesture_t;

#define CONSOLE_TAP_DEFAULT_MS   50u
#define CONSOLE_PRESS_DEFAULT_MS 1000u
#define CONSOLE_GESTURE_MAX_MS   60000u

static inline bool
console_press_parse(const char* args, console_touch_gesture_t* out) {
    int32_t x, y;
    uint32_t ms = CONSOLE_PRESS_DEFAULT_MS;
    if (!console_i32(&args, &x) || !console_i32(&args, &y)
        || (*args != '\0' && !console_u32(&args, CONSOLE_GESTURE_MAX_MS, false, &ms)) || *args != '\0' || ms == 0) {
        return false;
    }
    out->from = out->to = (vec2i_t){x, y};
    out->ms = ms;
    return true;
}

static inline bool
console_tap_parse(const char* args, console_touch_gesture_t* out) {
    int32_t x, y;
    if (!console_i32(&args, &x) || !console_i32(&args, &y) || *args != '\0') {
        return false;
    }
    *out = (console_touch_gesture_t){.from = {x, y}, .to = {x, y}, .ms = CONSOLE_TAP_DEFAULT_MS};
    return true;
}

static inline bool
console_drag_parse(const char* args, console_touch_gesture_t* out) {
    int32_t x0, y0, x1, y1;
    uint32_t ms;
    if (!console_i32(&args, &x0) || !console_i32(&args, &y0) || !console_i32(&args, &x1) || !console_i32(&args, &y1)
        || !console_u32(&args, CONSOLE_GESTURE_MAX_MS, false, &ms) || *args != '\0' || ms == 0) {
        return false;
    }
    out->from = (vec2i_t){x0, y0};
    out->to = (vec2i_t){x1, y1};
    out->ms = ms;
    return true;
}

typedef enum {
    CONSOLE_BUTTON_BOOT,
    CONSOLE_BUTTON_POWER,
} console_button_t;

static inline bool
console_button_parse(const char* args, console_button_t* button, bool* held) {
    char name[6];
    char kind[6] = "short";
    char trailing;
    const int fields = sscanf(args, "%5s %5s %c", name, kind, &trailing);
    if ((fields != 1 && fields != 2) || (strcmp(name, "boot") != 0 && strcmp(name, "power") != 0)
        || (strcmp(kind, "short") != 0 && strcmp(kind, "long") != 0)) {
        return false;
    }
    *button = strcmp(name, "boot") == 0 ? CONSOLE_BUTTON_BOOT : CONSOLE_BUTTON_POWER;
    *held = strcmp(kind, "long") == 0;
    return true;
}
