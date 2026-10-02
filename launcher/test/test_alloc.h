/* Aligned test buffers: the device's malloc gives 8 bytes, the host's 16, and
 * MinGW has no aligned_alloc. */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

/* `size` bytes at an `align`-aligned address (a power of two); NULL when out of
 * memory. Release with test_free_aligned(*raw_out). */
static inline void*
test_alloc_aligned(size_t size, size_t align, void** raw_out) {
    void* raw = malloc(size + align - 1U);
    *raw_out = raw;
    if (raw == NULL) {
        return NULL;
    }
    return (void*)(((uintptr_t)raw + align - 1U) & ~((uintptr_t)align - 1U));
}

static inline void
test_free_aligned(void* raw) {
    free(raw);
}
