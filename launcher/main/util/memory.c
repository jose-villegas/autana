/* memory: memory.h's size queries, over the heap this build has. */

#include "util/memory.h"

#include <stdio.h>

#if MEMORY_HEAP_CAPS

size_t
memory_free_bytes(memory_kind_t kind) {
    return heap_caps_get_free_size(memory_caps(kind));
}

size_t
memory_largest_block(memory_kind_t kind) {
    return heap_caps_get_largest_free_block(memory_caps(kind));
}

size_t
memory_total_bytes(memory_kind_t kind) {
    return heap_caps_get_total_size(memory_caps(kind));
}

void
memory_dump(memory_kind_t kind) {
    heap_caps_dump(memory_caps(kind));
}

#else

size_t
memory_free_bytes(memory_kind_t kind) {
    (void)kind;
    return SIZE_MAX;
}

size_t
memory_largest_block(memory_kind_t kind) {
    (void)kind;
    return SIZE_MAX;
}

size_t
memory_total_bytes(memory_kind_t kind) {
    (void)kind;
    return SIZE_MAX;
}

void
memory_dump(memory_kind_t kind) {
    (void)kind;
    printf("memory: the C library's heap, with no regions to report\n");
}

#endif
