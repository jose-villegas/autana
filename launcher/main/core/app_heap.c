/* app_heap: TLSF allocation and accounting for one app visit. */
#include "core/app_heap.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

/* IDF supplies these symbols on the board; hosts build its pinned upstream. */
#include "tlsf.h"
#include "tlsf_block_functions.h"

static tlsf_t heap;
static pool_t pool;
static uintptr_t region_start;
static size_t region_bytes;
static size_t total_bytes;
static size_t free_bytes;
static size_t minimum_bytes;
static size_t blocks;

void
app_heap_enter(void* region, size_t bytes) {
    assert(heap == NULL);
    assert(region != NULL);
    heap = tlsf_create(region, bytes);
    assert(heap != NULL);
    /* The pinned TLSF uses four-byte pool alignment even on 64-bit hosts.
     * Keep pool headers and split blocks aligned for native pointers. */
    const size_t align = _Alignof(max_align_t);
    const size_t offset = (tlsf_size(heap) + align - 1) & ~(align - 1);
    const size_t pool_bytes = (bytes - offset) & ~(sizeof(void*) - 1);
    pool = tlsf_add_pool(heap, (unsigned char*)region + offset, pool_bytes);
    assert(pool != NULL);
    region_start = (uintptr_t)region;
    region_bytes = bytes;
    total_bytes = pool_bytes;
    free_bytes = total_bytes;
    minimum_bytes = free_bytes;
    blocks = 0;
}

void
app_heap_exit(void) {
    assert(heap != NULL);
    tlsf_destroy(heap);
    heap = NULL;
}

bool
app_heap_active(void) {
    return heap != NULL;
}

bool
app_heap_contains(const void* pointer) {
    const uintptr_t address = (uintptr_t)pointer;
    return address >= region_start && address - region_start < region_bytes;
}

void*
app_heap_alloc(size_t bytes) {
    assert(heap != NULL);
    if (bytes == 0 || bytes > total_bytes) {
        return NULL;
    }
    const size_t rounded = (bytes + sizeof(void*) - 1) & ~(sizeof(void*) - 1);
    void* pointer = tlsf_memalign(heap, _Alignof(max_align_t), rounded);
    if (pointer != NULL) {
        free_bytes -= tlsf_block_size(pointer) + tlsf_alloc_overhead();
        if (free_bytes < minimum_bytes) {
            minimum_bytes = free_bytes;
        }
        blocks++;
    }
    return pointer;
}

void
app_heap_free(void* pointer) {
    assert(heap != NULL);
    free_bytes += tlsf_block_size(pointer) + tlsf_alloc_overhead();
    blocks--;
    tlsf_free(heap, pointer);
}

size_t
app_heap_free_bytes(void) {
    assert(heap != NULL);
    return free_bytes;
}

size_t
app_heap_total_bytes(void) {
    assert(heap != NULL);
    return total_bytes;
}

static bool
largest_block(void* pointer, size_t bytes, int used, void* user) {
    (void)pointer;
    size_t* largest = user;
    if (!used && bytes > *largest) {
        *largest = bytes;
    }
    return true;
}

size_t
app_heap_largest_block(void) {
    assert(heap != NULL);
    size_t largest = 0;
    tlsf_walk_pool(pool, largest_block, &largest);
    /* memalign searches for room for a free header and alignment padding. */
    const size_t align = _Alignof(max_align_t);
    const size_t gap = sizeof(block_header_t) + align;
    const size_t fitted = tlsf_fit_size(heap, largest) & ~(align - 1);
    return fitted >= gap + block_size_min ? fitted - gap : 0;
}

size_t
app_heap_min_free_bytes(void) {
    assert(heap != NULL);
    return minimum_bytes;
}

void
app_heap_min_free_restart(void) {
    if (heap != NULL) {
        minimum_bytes = free_bytes;
    }
}

size_t
app_heap_blocks(void) {
    assert(heap != NULL);
    return blocks;
}

void
app_heap_dump(void) {
    assert(heap != NULL);
    printf("app heap: %zu free, %zu blocks, %zu largest\n", free_bytes, blocks, app_heap_largest_block());
}
