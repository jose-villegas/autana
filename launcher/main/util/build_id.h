#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define BUILD_ID_MAX      32
#define BUILD_ID_LINE_MAX 48

typedef enum {
    BUILD_CONSOLE_NONE,
    BUILD_CONSOLE_SCREENSHOT,
    BUILD_CONSOLE_RUNSUITE,
    BUILD_CONSOLE_BUILD_ID,
    BUILD_CONSOLE_TOUCH,
    BUILD_CONSOLE_IMU,
} build_console_command_t;

static inline int
build_id_format(char* out, size_t out_size, const char* commit, bool dirty, const char* variant) {
    return snprintf(out, out_size, "%s%s-%s", commit, dirty ? "-dirty" : "", variant);
}

static inline int
build_id_line(char* out, size_t out_size, const char* build_id) {
    return snprintf(out, out_size, "BUILD_ID=%s", build_id);
}

static inline build_console_command_t
build_console_command_parse(const char* line) {
    if (strcmp(line, "SCREENSHOT") == 0) {
        return BUILD_CONSOLE_SCREENSHOT;
    }
    if (strncmp(line, "RUNSUITE ", sizeof "RUNSUITE " - 1) == 0) {
        return BUILD_CONSOLE_RUNSUITE;
    }
    if (strcmp(line, "BUILDID") == 0) {
        return BUILD_CONSOLE_BUILD_ID;
    }
    if (strncmp(line, "TOUCH ", sizeof "TOUCH " - 1) == 0) {
        return BUILD_CONSOLE_TOUCH;
    }
    if (strncmp(line, "IMU ", sizeof "IMU " - 1) == 0) {
        return BUILD_CONSOLE_IMU;
    }
    return BUILD_CONSOLE_NONE;
}

/* "TOUCH DOWN <x> <y>" in panel pixels, or "TOUCH UP". False leaves the
 * outputs alone. */
static inline bool
build_console_parse_touch(const char* line, bool* down, int* x, int* y) {
    if (strcmp(line, "TOUCH UP") == 0) {
        *down = false;
        return true;
    }
    int px = 0, py = 0;
    char trailing = 0;
    if (sscanf(line, "TOUCH DOWN %d %d%c", &px, &py, &trailing) != 2 || px < 0 || py < 0) {
        return false;
    }
    *down = true;
    *x = px;
    *y = py;
    return true;
}

/* "IMU <ax> <ay> <az>", the accelerometer in raw counts. */
static inline bool
build_console_parse_imu(const char* line, int* ax, int* ay, int* az) {
    int a = 0, b = 0, c = 0;
    char trailing = 0;
    if (sscanf(line, "IMU %d %d %d%c", &a, &b, &c, &trailing) != 3 || a < INT16_MIN || a > INT16_MAX || b < INT16_MIN
        || b > INT16_MAX || c < INT16_MIN || c > INT16_MAX) {
        return false;
    }
    *ax = a;
    *ay = b;
    *az = c;
    return true;
}
