#include "app_memory.h"

size_t
app_internal_memory_leaked_bytes(size_t free_before, size_t free_after) {
    if (free_after >= free_before) {
        return 0;
    }
    const size_t kept = free_before - free_after;
    return kept <= APP_INTERNAL_MEMORY_TOLERANCE ? 0 : kept;
}
