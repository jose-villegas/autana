/* checkpoint: owned byte copies restored into storage that outlives the checkpoint. */
#pragma once

#include "build/build_variant.h"

#if BUILD_VARIANT_DEVELOPMENT_OR_HOST
#include <string.h>
#include "core/memory.h"

typedef struct checkpoint_block {
    struct checkpoint_block* next;
    void* destination;
    size_t bytes;
    unsigned char data[];
} checkpoint_block_t;

typedef struct {
    checkpoint_block_t* first;
    bool failed;
} checkpoint_t;

static inline void
checkpoint_capture(checkpoint_t* state, void* destination, size_t bytes) {
    if (destination == NULL || bytes == 0 || state->failed) {
        return;
    }
    checkpoint_block_t* block = memory_alloc(sizeof(*block) + bytes, MEMORY_PSRAM);
    if (block == NULL) {
        state->failed = true;
        return;
    }
    *block = (checkpoint_block_t){state->first, destination, bytes};
    memcpy(block->data, destination, bytes);
    state->first = block;
}

static inline void
checkpoint_restore(const checkpoint_t* state) {
    for (const checkpoint_block_t* b = state->first; b != NULL; b = b->next) {
        memcpy(b->destination, b->data, b->bytes);
    }
}

static inline void
checkpoint_clear(checkpoint_t* state) {
    while (state->first != NULL) {
        checkpoint_block_t* b = state->first;
        state->first = b->next;
        memory_free(b);
    }
    *state = (checkpoint_t){0};
}
#endif
