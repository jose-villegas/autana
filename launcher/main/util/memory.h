/*
 * memory: the heap by kind, so a caller above the drivers can place a buffer,
 * or measure what it kept or lost, without naming the allocator.
 *
 * On the board and in the host tests each kind maps to heap capabilities:
 * ESP-IDF's heap on the board, test/heap_arena.c's device-sized model
 * (HOST_HEAP_ARENA) on a host. Any other host build, a render or the editor,
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

/* The largest single block an allocation of this kind could still get: free
 * space alone does not say whether one big buffer fits. */
size_t memory_largest_block(memory_kind_t kind);

/* Everything this kind's heap regions hold, free or not. */
size_t memory_total_bytes(memory_kind_t kind);

/* Prints every region of this kind and its blocks to the console. */
void memory_dump(memory_kind_t kind);

#if MEMORY_HEAP_CAPS
static inline uint32_t
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
