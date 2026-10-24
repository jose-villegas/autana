/*
 * console_inject_parse: reading the args of the verbs that stand in for
 * touch, the IMU and the board buttons (see console_inject.c). Pure and
 * portable: no verb dispatch, no controller, just sscanf() and bounds.
 */
#pragma once

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
console_touch_parse(const char* args, bool* down, int* x, int* y) {
    char state[5];
    int px, py;

    if (sscanf(args, "%4s %d %d", state, &px, &py) != 3) {
        return false;
    }
    if (strcmp(state, "down") == 0) {
        *down = true;
    } else if (strcmp(state, "up") == 0) {
        *down = false;
    } else {
        return false;
    }
    *x = px;
    *y = py;
    return true;
}

/* Reads `<ax> <ay> <az>`, the accelerometer in raw counts. False, and
 * nothing written, for a malformed line or a count the sensor's 16 bits
 * could not give. */
static inline bool
console_imu_parse(const char* args, int* ax, int* ay, int* az) {
    int a, b, c;
    char trailing;

    if (sscanf(args, "%d %d %d%c", &a, &b, &c, &trailing) != 3) {
        return false;
    }
    if (a < INT16_MIN || a > INT16_MAX || b < INT16_MIN || b > INT16_MAX || c < INT16_MIN || c > INT16_MAX) {
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
    int quarter;
    char trailing;
    if (sscanf(args, "quarter %d %c", &quarter, &trailing) != 1 || quarter < 0 || quarter >= DISPLAY_QUARTER_COUNT) {
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
    int x0, y0;
    int x1, y1;
    uint32_t ms;
} console_touch_gesture_t;

#define CONSOLE_TAP_DEFAULT_MS   50u
#define CONSOLE_PRESS_DEFAULT_MS 1000u
#define CONSOLE_GESTURE_MAX_MS   60000u

static inline bool
console_press_parse(const char* args, console_touch_gesture_t* out) {
    int x, y;
    unsigned ms = 0;
    char trailing;
    const int fields = sscanf(args, "%d %d %u %c", &x, &y, &ms, &trailing);
    if (fields == 2 && sscanf(args, "%d %d %c", &x, &y, &trailing) != 2) {
        return false;
    }
    if (fields != 2 && fields != 3) {
        return false;
    }
    if (fields == 2) {
        ms = CONSOLE_PRESS_DEFAULT_MS;
    }
    if (ms == 0 || ms > CONSOLE_GESTURE_MAX_MS) {
        return false;
    }
    out->x0 = x;
    out->y0 = y;
    out->x1 = x;
    out->y1 = y;
    out->ms = ms;
    return true;
}

static inline bool
console_tap_parse(const char* args, console_touch_gesture_t* out) {
    int x, y;
    char trailing;
    if (sscanf(args, "%d %d %c", &x, &y, &trailing) != 2) {
        return false;
    }
    *out = (console_touch_gesture_t){.x0 = x, .y0 = y, .x1 = x, .y1 = y, .ms = CONSOLE_TAP_DEFAULT_MS};
    return true;
}

static inline bool
console_drag_parse(const char* args, console_touch_gesture_t* out) {
    int x0, y0, x1, y1;
    unsigned ms;
    char trailing;
    if (sscanf(args, "%d %d %d %d %u %c", &x0, &y0, &x1, &y1, &ms, &trailing) != 5 || ms == 0
        || ms > CONSOLE_GESTURE_MAX_MS) {
        return false;
    }
    out->x0 = x0;
    out->y0 = y0;
    out->x1 = x1;
    out->y1 = y1;
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
