/*
 * heap_plain: heap_caps_*() over the C library's malloc(), for a host program
 * that renders rather than tests. A render needs its buffers to exist, not to
 * fit a budget, so nothing here models the board's pools; test/heap_arena.c
 * does that for the host tests. A host has no capacity to report, so every
 * size query answers SIZE_MAX.
 */
#include "stubs/esp_heap_caps.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

void*
heap_caps_malloc(size_t size, uint32_t caps) {
    (void)caps;
    return malloc(size);
}

void*
heap_caps_calloc(size_t n, size_t size, uint32_t caps) {
    (void)caps;
    return calloc(n, size);
}

void
heap_caps_free(void* ptr) {
    free(ptr);
}

size_t
heap_caps_get_free_size(uint32_t caps) {
    (void)caps;
    return SIZE_MAX;
}

size_t
heap_caps_get_largest_free_block(uint32_t caps) {
    (void)caps;
    return SIZE_MAX;
}

size_t
heap_caps_get_total_size(uint32_t caps) {
    (void)caps;
    return SIZE_MAX;
}

void
heap_caps_dump(uint32_t caps) {
    (void)caps;
    printf("heap_plain: the C library's heap, with no pools to report\n");
}
