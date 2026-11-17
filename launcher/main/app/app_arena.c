/* app_arena: chunked bump allocation from the visit's app heap. */
#include "app/app_arena.h"

#include <assert.h>
#include <stdint.h>

#include "core/memory.h"

typedef struct arena_chunk {
    struct arena_chunk* previous;
    size_t start;
    size_t capacity;
    unsigned char data[];
} arena_chunk_t;

static unsigned char* block;
static size_t block_bytes;
static size_t used;
static arena_chunk_t* current;

static size_t
psram_heap_bytes(void) {
#if MEMORY_HEAP_CAPS
    return memory_total_bytes(MEMORY_PSRAM);
#else
    return APP_ARENA_HOST_PSRAM_BYTES;
#endif
}

bool
app_arena_init(void) {
    assert(block == NULL);
    const size_t heap = psram_heap_bytes();
    if (heap <= APP_ARENA_SYSTEM_RESERVE_BYTES) {
        return false;
    }
#if MEMORY_HEAP_CAPS
    const size_t bytes = memory_unrounded_bytes(heap - APP_ARENA_SYSTEM_RESERVE_BYTES);
#else
    const size_t bytes = heap - APP_ARENA_SYSTEM_RESERVE_BYTES;
#endif
    /* TLSF aligns its own blocks inside the region. */
    block = memory_alloc(bytes, MEMORY_PSRAM);
    if (block == NULL) {
        return false;
    }
    block_bytes = bytes;
    return true;
}

size_t
app_arena_bytes(void) {
    return block_bytes;
}

void
app_arena_enter(void) {
    assert(current == NULL && used == 0);
    app_heap_enter(block, block_bytes);
}

void
app_arena_exit(void) {
    app_arena_rewind(0);
    app_heap_exit();
}

void*
app_arena_take(size_t size, size_t align) {
    if (block == NULL || size == 0 || align == 0 || (align & (align - 1)) != 0 || size > SIZE_MAX - align) {
        return NULL;
    }
    if (current != NULL) {
        const uintptr_t base = (uintptr_t)current->data;
        const size_t offset = used - current->start;
        const size_t padding = (-(base + offset)) & (align - 1);
        if (padding <= current->capacity - offset && size <= current->capacity - offset - padding) {
            used += padding + size;
            return current->data + offset + padding;
        }
    }
    const size_t requested = size + align;
    const size_t capacity = requested > APP_ARENA_CHUNK_BYTES ? requested : APP_ARENA_CHUNK_BYTES;
    if (capacity > SIZE_MAX - sizeof(arena_chunk_t)) {
        return NULL;
    }
    arena_chunk_t* chunk = app_heap_lend(sizeof *chunk + capacity);
    if (chunk == NULL) {
        return NULL;
    }
    *chunk = (arena_chunk_t){.previous = current, .start = used, .capacity = capacity};
    const size_t padding = (-(uintptr_t)chunk->data) & (align - 1);
    current = chunk;
    used += padding + size;
    return chunk->data + padding;
}

size_t
app_arena_mark(void) {
    return used;
}

void
app_arena_rewind(size_t mark) {
    assert(mark <= used);
    while (current != NULL && mark <= current->start) {
        arena_chunk_t* previous = current->previous;
        app_heap_reclaim(current);
        current = previous;
    }
    used = mark;
}
