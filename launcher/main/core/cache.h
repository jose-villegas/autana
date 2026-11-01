/*
 * cache: scoped control of chip-wide data-cache autoload.
 * Both cores, including audio, input and present, run under the pushed region.
 * Never call from an ISR or a timing-critical driver path. Callers serialize
 * scopes across cores and unwind nested pushes in reverse order.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "build/build_variant.h"

typedef struct {
    uint32_t ctrl, addr, size;
} cache_autoload_state_t;

/* On a miss inside [addr, addr + size), hardware also fetches following lines.
 * Pop restores the registers found by push. */
#if defined(ESP_PLATFORM) && !CONFIG_LAUNCHER_QEMU
cache_autoload_state_t cache_autoload_push(const void* addr, size_t size);
void cache_autoload_pop(cache_autoload_state_t previous);
#else
static inline cache_autoload_state_t
cache_autoload_push(const void* addr, size_t size) {
    (void)addr;
    (void)size;
    return (cache_autoload_state_t){0};
}

static inline void
cache_autoload_pop(cache_autoload_state_t previous) {
    (void)previous;
}
#endif
