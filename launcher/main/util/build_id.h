#pragma once

#include <stddef.h>
#include <stdio.h>

#define BUILD_ID_HASH_CHARS  12
#define BUILD_ID_SHORT_CHARS 7
#define BUILD_ID_LINE_MAX    48

const char* build_id(void);
const char* build_id_short(void);

static inline int
build_id_line(char* out, size_t out_size, const char* build_id) {
    return snprintf(out, out_size, "BUILD_ID=%s", build_id);
}
