/* memory: memory.h's size queries, over the board's heap or its host model. */

#include "core/memory.h"

#include "esp_memory_utils.h"
#include "esp_psram.h"

size_t
memory_free_bytes(memory_kind_t kind) {
    if (kind == MEMORY_APP) {
        return app_heap_free_bytes();
    }
    return heap_caps_get_free_size(memory_caps(kind));
}

size_t
memory_min_free_bytes(memory_kind_t kind) {
    if (kind == MEMORY_APP) {
        return app_heap_min_free_bytes();
    }
    return heap_caps_get_minimum_free_size(memory_caps(kind));
}

void
memory_min_free_restart(void) {
    app_heap_min_free_restart();
    (void)heap_caps_monitor_local_minimum_free_size_stop(); /* fails only when none was running */
    (void)heap_caps_monitor_local_minimum_free_size_start();
}

size_t
memory_largest_block(memory_kind_t kind) {
    if (kind == MEMORY_APP) {
        return app_heap_largest_block();
    }
    return heap_caps_get_largest_free_block(memory_caps(kind));
}

size_t
memory_total_bytes(memory_kind_t kind) {
    if (kind == MEMORY_APP) {
        return app_heap_total_bytes();
    }
    return heap_caps_get_total_size(memory_caps(kind));
}

size_t
memory_psram_bytes(void) {
    return esp_psram_get_size();
}

bool
memory_in_psram(const void* p) {
    return esp_ptr_external_ram(p);
}

void
memory_dump(memory_kind_t kind) {
    if (kind == MEMORY_APP) {
        app_heap_dump();
        return;
    }
    heap_caps_dump(memory_caps(kind));
}
