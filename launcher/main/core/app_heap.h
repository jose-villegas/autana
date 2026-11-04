/* app_heap: the visit's TLSF heap inside the boot-owned PSRAM region. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

void app_heap_enter(void* region, size_t bytes);
void app_heap_exit(void);
bool app_heap_active(void);
bool app_heap_contains(const void* pointer);
void* app_heap_alloc(size_t bytes);
void app_heap_free(void* pointer);

/* app_heap_alloc() and app_heap_free() for a lender, the app arena, out of
 * the trace hook's sight: a scene that loads through the arena is no heap
 * work per frame, as it was none from the fixed block the arena replaced. */
void* app_heap_lend(size_t bytes);
void app_heap_reclaim(void* pointer);
size_t app_heap_free_bytes(void);
size_t app_heap_total_bytes(void);
size_t app_heap_largest_block(void);
size_t app_heap_min_free_bytes(void);
void app_heap_min_free_restart(void);
size_t app_heap_blocks(void);
void app_heap_dump(void);

/* Called after each allocation (`alloc` true) and free, with `site` the
 * caller's return address: ESP-IDF's heap hooks do not see this heap. A
 * no-op unless something above defines it, as ESP-IDF's
 * esp_heap_trace_alloc_hook() is. */
void app_heap_trace_hook(bool alloc, const void* site);
