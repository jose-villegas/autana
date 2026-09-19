#pragma once

#include <stdbool.h>
#include <stddef.h>
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
    return BUILD_CONSOLE_NONE;
}

/* Reads `TOUCH <down|up> <x> <y>` into its three parts, in panel
 * coordinates. Returns false and writes nothing for a malformed line: a
 * caller acts on a whole sample or on none of it, never on half of one.
 * Coordinates are not range-checked here - the panel's size is not this
 * header's to know, and a sample past the edge is a caller's question. */
static inline bool
build_console_touch_parse(const char* line, bool* down, int* x, int* y) {
    const char* rest = line + sizeof "TOUCH " - 1;
    char state[5];
    int px, py;

    if (sscanf(rest, "%4s %d %d", state, &px, &py) != 3) {
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
