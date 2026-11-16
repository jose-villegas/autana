/* mem: aligned slices of caller-owned storage. */
#pragma once

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

/* Alignment is a nonzero power of two; the resulting end must fit size_t. */
static inline size_t
mem_carve(size_t* end, size_t bytes, size_t align) {
    assert(align != 0 && (align & (align - 1)) == 0);
    assert(*end <= SIZE_MAX - (align - 1));
    const size_t at = (*end + align - 1) & ~(align - 1);
    assert(bytes <= SIZE_MAX - at);
    *end = at + bytes;
    return at;
}
