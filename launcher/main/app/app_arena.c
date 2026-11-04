/*
 * app_arena: see app_arena.h. Portable: on a host the region comes from the
 * modeled PSRAM heap, or from malloc() sized from the device profile, so the
 * allocator is tested there as it runs on the board.
 */
#include "app/app_arena.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

#include "core/memory.h"

/* A cache line, so the region's first take shares none with the heap's. */
#define REGION_ALIGN 64u

static unsigned char* block;
static size_t block_bytes;
static size_t used;

static bool
is_power_of_two(size_t n) {
    return n != 0 && (n & (n - 1)) == 0;
}

/* The heap the region is carved from. A host without the modeled heap has no
 * size queries, so its build passes the device profile's PSRAM size. */
static size_t
psram_heap_bytes(void) {
#if MEMORY_HEAP_CAPS
    return memory_total_bytes(MEMORY_PSRAM);
#else
    return APP_ARENA_HOST_PSRAM_BYTES;
#endif
}

bool
app_arena_init(void) {
    assert(block == NULL);
    const size_t heap = psram_heap_bytes();
    if (heap <= APP_ARENA_SYSTEM_RESERVE_BYTES) {
        return false;
    }
    const size_t bytes = heap - APP_ARENA_SYSTEM_RESERVE_BYTES;
    const memory_aligned_t region = memory_alloc_aligned(bytes, REGION_ALIGN, MEMORY_PSRAM);
    if (region.at == NULL) {
        return false;
    }
    block = region.at;
    block_bytes = bytes;
    return true;
}

size_t
app_arena_bytes(void) {
    return block_bytes;
}

void*
app_arena_take(size_t size, size_t align) {
    if (size == 0 || !is_power_of_two(align) || block == NULL) {
        return NULL;
    }
    const uintptr_t base = (uintptr_t)block;
    const uintptr_t start = (base + used + (align - 1)) & ~(uintptr_t)(align - 1);
    const size_t offset = (size_t)(start - base);
    if (offset > block_bytes || size > block_bytes - offset) {
        return NULL;
    }
    used = offset + size;
    return &block[offset];
}

size_t
app_arena_mark(void) {
    return used;
}

void
app_arena_rewind(size_t mark) {
    assert(mark <= used);
    used = mark;
}
