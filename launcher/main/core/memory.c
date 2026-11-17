/* memory: memory.h's size queries, over the board's heap or its host model. */

#include "core/memory.h"

#include <limits.h>
#include <stdio.h>

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

/* The size classes per power of two in a TLSF heap over 256 KB. */
#define TLSF_CLASSES_LOG2 5u

size_t
memory_unrounded_bytes(size_t bytes) {
    if (bytes == 0) {
        return 0;
    }
    const unsigned long long wide = bytes;
    const size_t power = (size_t)1 << (sizeof wide * CHAR_BIT - 1 - (size_t)__builtin_clzll(wide));
    const size_t interval = power >> TLSF_CLASSES_LOG2;
    return interval == 0 ? bytes : bytes & ~(interval - 1);
}

size_t
memory_block_bytes(const void* block) {
    if (app_heap_contains(block)) {
        return app_heap_block_bytes(block);
    }
    return heap_caps_get_allocated_size((void*)block);
}

size_t
memory_psram_bytes(void) {
    return esp_psram_get_size();
}

bool
memory_in_psram(const void* p) {
    return esp_ptr_external_ram(p);
}

/* The first call sites MEMORY_FAST fell back at, each counted; one past
 * them counts only in the total. Either core may allocate, so each count is
 * atomic and a site claims its slot by compare-and-swap. */
#define MEMORY_FALLBACK_SITES 8

typedef struct {
    const void* site;
    memory_fallbacks_t taken;
} fallback_site_t;

static fallback_site_t fallback_sites[MEMORY_FALLBACK_SITES];
static memory_fallbacks_t fallback_total;

static void
count(memory_fallbacks_t* into, size_t bytes) {
    (void)__atomic_fetch_add(&into->count, 1, __ATOMIC_RELAXED);
    (void)__atomic_fetch_add(&into->bytes, bytes, __ATOMIC_RELAXED);
}

__attribute__((noinline)) void*
memory_fast_fallback(size_t bytes) {
    const void* site = __builtin_return_address(0);
    void* block = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (block == NULL) {
        return NULL;
    }
    count(&fallback_total, bytes);
    for (size_t i = 0; i < MEMORY_FALLBACK_SITES; i++) {
        const void* seen = NULL;
        if (__atomic_compare_exchange_n(&fallback_sites[i].site, &seen, site, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)
            || seen == site) {
            count(&fallback_sites[i].taken, bytes);
            break;
        }
    }
    return block;
}

memory_fallbacks_t
memory_fast_fallbacks(void) {
    return (memory_fallbacks_t){__atomic_load_n(&fallback_total.count, __ATOMIC_RELAXED),
                                __atomic_load_n(&fallback_total.bytes, __ATOMIC_RELAXED)};
}

void
memory_dump(memory_kind_t kind) {
    if (kind == MEMORY_APP) {
        app_heap_dump();
    } else {
        heap_caps_dump(memory_caps(kind));
    }
    const memory_fallbacks_t total = memory_fast_fallbacks();
    (void)printf("MEMORY_FAST fell back to external RAM %u times, %zu bytes\n", (unsigned)total.count, total.bytes);
    for (size_t i = 0; i < MEMORY_FALLBACK_SITES; i++) {
        const void* site = __atomic_load_n(&fallback_sites[i].site, __ATOMIC_RELAXED);
        if (site != NULL) {
            (void)printf("  at %p: %u times, %zu bytes\n", site, (unsigned)fallback_sites[i].taken.count,
                         fallback_sites[i].taken.bytes);
        }
    }
}
