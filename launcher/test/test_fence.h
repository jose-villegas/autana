/* Host-only test buffers that end where an unreadable page begins, so a
 * parser that reads one byte past its input faults instead of reading what
 * happens to follow. */
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t* bytes;
    void* region;
    size_t region_size;
} test_fence_t;

/* `size` bytes ending at the unreadable page; a multiple of 16 keeps them
 * 16-byte aligned. `bytes` is NULL when the OS refuses. */
test_fence_t test_fence_alloc(size_t size);

void test_fence_free(test_fence_t* fence);
