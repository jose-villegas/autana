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
    return BUILD_CONSOLE_NONE;
}
