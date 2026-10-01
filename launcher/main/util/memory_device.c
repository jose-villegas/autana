/* memory_device: memory.h over the capability-aware heap. */

#include "util/memory.h"

#include <stdint.h>

#include "esp_heap_caps.h"

static uint32_t
capabilities(memory_kind_t kind) {
    switch (kind) {
        case MEMORY_INTERNAL: return MALLOC_CAP_INTERNAL;
        case MEMORY_8BIT: return MALLOC_CAP_8BIT;
        case MEMORY_DMA: return MALLOC_CAP_DMA;
    }
    return MALLOC_CAP_INTERNAL;
}

size_t
memory_free_bytes(memory_kind_t kind) {
    return heap_caps_get_free_size(capabilities(kind));
}

size_t
memory_largest_block(memory_kind_t kind) {
    return heap_caps_get_largest_free_block(capabilities(kind));
}

void
memory_dump(memory_kind_t kind) {
    heap_caps_dump(capabilities(kind));
}
