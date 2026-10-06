/*
 * build_id: this image's identity, its ELF hash and build variant, and the
 * BUILD_ID= line a host matches it by.
 */
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
