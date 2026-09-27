/*
 * app_arena - see app_arena.h. Portable: on a host the block is ordinary
 * static memory, so the allocator is tested there as it runs on the board.
 */
#include "app_arena.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

#if defined(ESP_PLATFORM)
#include "esp_attr.h"
#include "sdkconfig.h"
#if !CONFIG_SPIRAM_ALLOW_NOINIT_SEG_EXTERNAL_MEMORY
#error "app_arena needs CONFIG_SPIRAM_ALLOW_NOINIT_SEG_EXTERNAL_MEMORY, or its block lands in internal RAM"
#endif
#define APP_ARENA_PLACEMENT EXT_RAM_NOINIT_ATTR
#else
#define APP_ARENA_PLACEMENT
#endif

APP_ARENA_PLACEMENT static _Alignas(64) unsigned char block[APP_ARENA_BYTES];
static size_t used;

static bool
is_power_of_two(size_t n) {
    return n != 0 && (n & (n - 1)) == 0;
}

void*
app_arena_take(size_t size, size_t align) {
    if (size == 0 || !is_power_of_two(align)) {
        return NULL;
    }
    const uintptr_t base = (uintptr_t)block;
    const uintptr_t start = (base + used + (align - 1)) & ~(uintptr_t)(align - 1);
    const size_t offset = (size_t)(start - base);
    if (offset > APP_ARENA_BYTES || size > APP_ARENA_BYTES - offset) {
        return NULL;
    }
    used = offset + size;
    return &block[offset];
}

size_t
app_arena_mark(void) {
    return used;
}

void
app_arena_rewind(size_t mark) {
    assert(mark <= used);
    used = mark;
}
