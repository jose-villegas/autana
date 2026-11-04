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
size_t app_heap_free_bytes(void);
size_t app_heap_total_bytes(void);
size_t app_heap_largest_block(void);
size_t app_heap_min_free_bytes(void);
void app_heap_min_free_restart(void);
size_t app_heap_blocks(void);
void app_heap_dump(void);
