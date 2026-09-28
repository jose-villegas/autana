/* App internal-heap lifetime accounting. */
#pragma once

#include <stddef.h>

/* Heap metadata can vary by a few words across a lifecycle. */
#define APP_INTERNAL_MEMORY_TOLERANCE 64u

size_t app_internal_memory_leaked_bytes(size_t free_before, size_t free_after);
