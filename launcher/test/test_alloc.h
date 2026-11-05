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

/* Holds every block the app heap can still give, chained through the blocks
 * themselves; test_app_heap_release() gives them back. */
static inline void*
test_app_heap_fill(void) {
    void* head = NULL;
    size_t largest;
    while ((largest = memory_largest_block(MEMORY_APP)) >= sizeof(void*)) {
        void* block = memory_alloc(largest, MEMORY_APP);
        if (block == NULL) {
            break;
        }
        *(void**)block = head;
        head = block;
    }
    return head;
}

static inline void
test_app_heap_release(void* head) {
    while (head != NULL) {
        void* next = *(void**)head;
        memory_free(head);
        head = next;
    }
}
