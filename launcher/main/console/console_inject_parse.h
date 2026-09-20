/*
 * console_inject_parse - reading a TOUCH or IMU verb's args, the two
 * CONFIG_LAUNCHER_QEMU console verbs a host script uses to stand in for
 * hardware QEMU has none of (see console_inject.c). Pure and portable: no
 * verb dispatch, no controller, just two lines' worth of sscanf().
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Reads `<down|up> <x> <y>` into its three parts, in panel coordinates.
 * Returns false and writes nothing for a malformed line: a caller acts on a
 * whole sample or on none of it, never on half of one. Coordinates are not
 * range-checked here - the panel's size is not this header's to know, and a
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
