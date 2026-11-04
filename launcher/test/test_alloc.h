/* test_alloc: visit-owned aligned buffers and arena exhaustion for test fixtures. */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include "app/app_arena.h"
#include "core/memory.h"

/* `size` bytes at an `align`-aligned address (a power of two); NULL when out of
 * memory. Release with test_free_aligned(*raw_out). */
static inline void*
test_alloc_aligned(size_t size, size_t align, void** raw_out) {
    const memory_aligned_t block = memory_alloc_aligned(size, align, MEMORY_APP);
    *raw_out = block.block;
    return block.at;
}

static inline void
test_free_aligned(void* raw) {
    memory_free(raw);
}

static inline void
test_arena_fill(void) {
    while (app_arena_take(APP_ARENA_CHUNK_BYTES, 1) != NULL) {}
    while (app_arena_take(1, 1) != NULL) {}
}
