/*
 * render_lab_render_host_heap - a plain-malloc heap_caps_*() for this app's
 * render_host.h scenes, matching the declarations test/stubs/esp_heap_caps.h
 * checks scene_wire.c against. test/heap_arena.c models the device's real
 * heap caps, but its malloc/calloc/realloc/free wrapping reaches gfx_init()'s
 * own host branch (gfx.c), which allocates the framebuffer with a plain
 * malloc() it expects to always succeed - a rendering preview needs
 * scene_wire.c's small scratch buffers to allocate, nothing about their size
 * or placement.
 */
#include "../../../../test/stubs/esp_heap_caps.h"

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
    return (size_t)-1; /* a host has no real cap to report */
}

size_t
heap_caps_get_largest_free_block(uint32_t caps) {
    (void)caps;
    return (size_t)-1;
}
