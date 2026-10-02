/*
 * memory: how much heap is left, by kind, so a caller above the drivers can
 * measure what it kept or lost without naming the allocator. Defined in
 * memory_device.c, device only.
 */
#pragma once

#include <stddef.h>

typedef enum {
    MEMORY_INTERNAL, /* on-chip RAM */
    MEMORY_8BIT,     /* anything a byte access reaches, internal or external */
    MEMORY_DMA,      /* what the panel and bus transfers can read from */
} memory_kind_t;

size_t memory_free_bytes(memory_kind_t kind);

/* The largest single block an allocation of this kind could still get: free
 * space alone does not say whether one big buffer fits. */
size_t memory_largest_block(memory_kind_t kind);

/* Prints every region of this kind and its blocks to the console. */
void memory_dump(memory_kind_t kind);
