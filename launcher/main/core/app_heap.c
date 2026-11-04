/* app_heap: TLSF allocation and accounting for one app visit.
 *
 * Rejected: ESP-IDF's multi_heap_register() over the region. It would answer
 * the size queries on the board, but a registered heap cannot be dropped, so
 * the region could not start each visit empty, and a host has no heap_caps
 * registry to run it in. */
#include "core/app_heap.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

/* IDF supplies these symbols on the board; hosts build its pinned upstream. */
#include "tlsf.h"
#include "tlsf_block_functions.h"

/* What a block's address is a multiple of. On the board, TLSF's own four
 * bytes, as ESP-IDF's PSRAM heap gives: no padding gap to split off. A host
 * wants what its malloc() gives, which TLSF only gives through memalign. */
#ifdef ESP_PLATFORM
#define BLOCK_ALIGN 4u
#else
#define BLOCK_ALIGN _Alignof(max_align_t)
#endif

/* Free space is counted as ESP-IDF's multi_heap counts it: the pool less
 * each live block and its header, not the free blocks' own headers, so a
 * MEMORY_APP reading compares with a MEMORY_PSRAM one. */
static tlsf_t heap;
static pool_t pool;
static uintptr_t region_start;
static size_t region_bytes;
static size_t total_bytes;
static size_t free_bytes;
static size_t minimum_bytes;
static size_t blocks;

__attribute__((weak)) void
app_heap_trace_hook(bool alloc, const void* site) {
    (void)alloc;
    (void)site;
}

void
app_heap_enter(void* region, size_t bytes) {
    assert(heap == NULL);
    assert(region != NULL);
    heap = tlsf_create(region, bytes);
    assert(heap != NULL);
    /* The pinned TLSF uses four-byte pool alignment even on 64-bit hosts.
     * Keep pool headers and split blocks aligned for native pointers. */
    const size_t align = BLOCK_ALIGN;
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
    _Static_assert(BLOCK_ALIGN >= 4u, "TLSF aligns to four bytes at least");
    const size_t rounded = (bytes + sizeof(void*) - 1) & ~(sizeof(void*) - 1);
    void* pointer = BLOCK_ALIGN == 4u ? tlsf_malloc(heap, rounded) : tlsf_memalign(heap, BLOCK_ALIGN, rounded);
    if (pointer != NULL) {
        free_bytes -= tlsf_block_size(pointer) + tlsf_alloc_overhead();
        if (free_bytes < minimum_bytes) {
            minimum_bytes = free_bytes;
        }
        blocks++;
        /* memory_alloc() is inline, so this return address is its caller's. */
        app_heap_trace_hook(true, __builtin_return_address(0));
    }
    return pointer;
}

void
app_heap_free(void* pointer) {
    assert(heap != NULL && "a MEMORY_APP block freed after its visit ended");
    free_bytes += tlsf_block_size(pointer) + tlsf_alloc_overhead();
    blocks--;
    tlsf_free(heap, pointer);
    app_heap_trace_hook(false, __builtin_return_address(0));
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
    if (BLOCK_ALIGN == 4u) {
        return tlsf_fit_size(heap, largest);
    }
    /* memalign searches for room for a free header and alignment padding. */
    const size_t align = BLOCK_ALIGN;
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
