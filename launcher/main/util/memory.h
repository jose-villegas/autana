/*
 * memory: the heap by kind, so a caller above the drivers can place a buffer,
 * or measure what it kept or lost, without naming the allocator.
 *
 * memory.c maps each kind to the heap's capabilities. The heap underneath is
 * ESP-IDF's on the board, test/heap_arena.c's device-sized model in the host
 * tests, and test/heap_plain.c's plain malloc() in a host render, which has
 * no budget to report.
 *
 * Rejected: a cap mask in the interface. A caller names what the buffer is
 * for; which capability bits mean that on this chip is this module's call.
 */
#pragma once

#include <stddef.h>

typedef enum {
    MEMORY_INTERNAL, /* on-chip RAM */
    MEMORY_8BIT,     /* anything a byte access reaches, internal or external */
    MEMORY_DMA,      /* what the panel and bus transfers can read from */
    MEMORY_PSRAM,    /* external RAM: large and slow, and no DMA reaches it */
} memory_kind_t;

size_t memory_free_bytes(memory_kind_t kind);

/* The largest single block an allocation of this kind could still get: free
 * space alone does not say whether one big buffer fits. */
size_t memory_largest_block(memory_kind_t kind);

/* Everything this kind's heap regions hold, free or not. */
size_t memory_total_bytes(memory_kind_t kind);

/* Prints every region of this kind and its blocks to the console. */
void memory_dump(memory_kind_t kind);

/* A byte-addressable block of this kind, or NULL when none is that large. A
 * request for one kind never falls back to another: running out where the
 * buffer was meant to live is the failure a caller has to see. */
void* memory_alloc(size_t bytes, memory_kind_t kind);

/* Returns a block from memory_alloc(); NULL is a no-op. */
void memory_free(void* block);
