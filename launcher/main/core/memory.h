/*
 * memory: the heap by kind, so a caller above the drivers can place a buffer,
 * or measure what it kept or lost, without naming the allocator.
 *
 * On the board and in the host tests each kind maps to heap capabilities:
 * ESP-IDF's heap on the board, test/heap_arena.c's device-sized model
 * (HOST_HEAP_ARENA) on a host. Any other host build, a render,
 * allocates from the C library's malloc(), which has no budget, so it has no
 * size queries either: memory.c is not built there.
 *
 * Allocating and freeing are inline: a heap watch names the first frame
 * outside the allocator as the call site, so a frame of this module's own
 * would make every caller one site.
 *
 * Rejected: a cap mask in the interface. A caller names what the buffer is
 * for; which capability bits mean that on this chip is this module's call.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(ESP_PLATFORM) || defined(HOST_HEAP_ARENA)
#include "esp_heap_caps.h"
#define MEMORY_HEAP_CAPS 1
#else
#include <stdlib.h>
#define MEMORY_HEAP_CAPS 0
#endif

typedef enum {
    MEMORY_INTERNAL, /* on-chip RAM */
    MEMORY_8BIT,     /* anything a byte access reaches, internal or external */
    MEMORY_DMA,      /* what the heap hands out as DMA-capable: internal only */
    MEMORY_PSRAM,    /* external RAM: large and slow */
} memory_kind_t;

size_t memory_free_bytes(memory_kind_t kind);

/* Each heap's own low-water mark since boot, summed: at or below the lowest
 * memory_free_bytes() ever was, since the heaps need not bottom out together.
 * Shows a transient allocation already freed again. */
size_t memory_min_free_bytes(memory_kind_t kind);

/* The largest single block an allocation of this kind could still get: free
 * space alone does not say whether one big buffer fits. */
size_t memory_largest_block(memory_kind_t kind);

/* Everything this kind's heap regions hold, free or not. */
size_t memory_total_bytes(memory_kind_t kind);

/* The external RAM chip's whole size, the part the board reserves ahead of
 * the heap included; 0 when the board has none. */
size_t memory_psram_bytes(void);

/* Whether `p` points into external RAM, where an allocation that asked for
 * no kind lands once internal RAM runs short. */
bool memory_in_psram(const void* p);

/* Prints every region of this kind and its blocks to the console. */
void memory_dump(memory_kind_t kind);

#if MEMORY_HEAP_CAPS
static inline __attribute__((always_inline)) uint32_t
memory_caps(memory_kind_t kind) {
    switch (kind) {
        case MEMORY_INTERNAL: return MALLOC_CAP_INTERNAL;
        case MEMORY_8BIT: return MALLOC_CAP_8BIT;
        case MEMORY_DMA: return MALLOC_CAP_DMA;
        case MEMORY_PSRAM: return MALLOC_CAP_SPIRAM;
    }
    return MALLOC_CAP_8BIT;
}
#endif

/* A byte-addressable block of this kind, or NULL when none is that large. A
 * request for one kind never falls back to another: running out where the
 * buffer was meant to live is the failure a caller has to see. */
static inline __attribute__((always_inline)) void*
memory_alloc(size_t bytes, memory_kind_t kind) {
#if MEMORY_HEAP_CAPS
    /* Internal RAM also holds word-only regions, which a byte store faults
     * in. */
    return heap_caps_malloc(bytes, memory_caps(kind) | MALLOC_CAP_8BIT);
#else
    (void)kind;
    return malloc(bytes);
#endif
}

/* Returns a block from memory_alloc(); NULL is a no-op. */
static inline __attribute__((always_inline)) void
memory_free(void* block) {
#if MEMORY_HEAP_CAPS
    heap_caps_free(block);
#else
    free(block);
#endif
}

/* A block of this kind holding `bytes` at an `align` boundary (a power of two):
 * `at` is the aligned start, `block` what memory_free() takes; both NULL when
 * none is that large. Over-allocates, so it works with every heap here. */
typedef struct {
    void* block;
    void* at;
} memory_aligned_t;

static inline __attribute__((always_inline)) memory_aligned_t
memory_alloc_aligned(size_t bytes, size_t align, memory_kind_t kind) {
    void* block = memory_alloc(bytes + align - 1, kind);
    return (memory_aligned_t){
        .block = block,
        .at = block == NULL ? NULL : (void*)(((uintptr_t)block + align - 1) & ~(uintptr_t)(align - 1)),
    };
}

/* A lender's take, as app/app_arena.h's app_arena_take(): the next `size`
 * bytes at `align`, or NULL. A taken block is never freed: its owner gives
 * back everything at once. NULL in place of one means the heap. */
typedef void* (*memory_take_fn)(size_t size, size_t align);
