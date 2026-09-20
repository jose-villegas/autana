#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#define BUILD_ID_MAX      32
#define BUILD_ID_LINE_MAX 48

static inline int
build_id_format(char* out, size_t out_size, const char* commit, bool dirty, const char* variant) {
    return snprintf(out, out_size, "%s%s-%s", commit, dirty ? "-dirty" : "", variant);
}

static inline int
build_id_line(char* out, size_t out_size, const char* build_id) {
    return snprintf(out, out_size, "BUILD_ID=%s", build_id);
}
