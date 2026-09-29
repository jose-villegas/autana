#pragma once

#include <stddef.h>
#include <stdio.h>

#define BUILD_ID_LINE_MAX 48

static inline int
build_id_line(char* out, size_t out_size, const char* build_id) {
    return snprintf(out, out_size, "BUILD_ID=%s", build_id);
}
