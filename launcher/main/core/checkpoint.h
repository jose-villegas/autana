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
    const void* owner;
    const void* partner_owner;
    void* partner_destination;
    unsigned char data[];
} checkpoint_block_t;

typedef struct checkpoint {
    checkpoint_block_t* first;
    checkpoint_block_t* last;
    bool failed;
} checkpoint_t;

typedef struct {
    void* destination;
    size_t bytes;
} checkpoint_field_t;

#define CHECKPOINT_CAPTURE(state, value) checkpoint_capture((state), (void*)&(value), sizeof(value))

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
    *block = (checkpoint_block_t){.destination = destination, .bytes = bytes};
    memcpy(block->data, destination, bytes);
    if (state->last == NULL) {
        state->first = block;
    } else {
        state->last->next = block;
    }
    state->last = block;
}

static inline void
checkpoint_capture_fields(checkpoint_t* state, const checkpoint_field_t* fields, size_t count) {
    for (size_t i = 0; i < count; i++) {
        checkpoint_capture(state, fields[i].destination, fields[i].bytes);
    }
}

static inline void*
checkpoint_pointer(const void* owner) {
    void* value;
    memcpy(&value, owner, sizeof value);
    return value;
}

static inline void
checkpoint_capture_at(checkpoint_t* state, const void* owner, size_t bytes) {
    void* destination = checkpoint_pointer(owner);
    checkpoint_capture(state, (void*)owner, sizeof(void*));
    if (!state->failed) {
        checkpoint_block_t* guard = state->last;
        guard->owner = owner;
        guard->destination = destination;
    }
    checkpoint_capture(state, destination, bytes);
}

static inline void
checkpoint_capture_pair(checkpoint_t* state, const void* owner, const void* partner, size_t bytes) {
    checkpoint_capture_at(state, owner, bytes);
    if (state->failed) {
        return;
    }
    for (checkpoint_block_t* b = state->first; b != NULL; b = b->next) {
        if (b->owner == owner) {
            b->partner_owner = partner;
            b->partner_destination = checkpoint_pointer(partner);
        }
    }
    void* other = checkpoint_pointer(partner);
    if (other != checkpoint_pointer(owner)) {
        checkpoint_capture(state, other, bytes);
    }
}

static inline bool
checkpoint_validate(const checkpoint_t* state) {
    if (state->failed) {
        return false;
    }
    for (const checkpoint_block_t* b = state->first; b != NULL; b = b->next) {
        if (b->owner == NULL) {
            continue;
        }
        const void* current = checkpoint_pointer(b->owner);
        if (b->partner_owner != NULL) {
            const void* partner = checkpoint_pointer(b->partner_owner);
            if ((current == b->destination && partner == b->partner_destination)
                || (current == b->partner_destination && partner == b->destination)) {
                continue;
            }
            return false;
        }
        if (current != b->destination) {
            return false;
        }
    }
    return true;
}

static inline bool
checkpoint_restore(const checkpoint_t* state) {
    if (!checkpoint_validate(state)) {
        return false;
    }
    for (const checkpoint_block_t* b = state->first; b != NULL; b = b->next) {
        if (b->owner == NULL) {
            memcpy(b->destination, b->data, b->bytes);
        }
    }
    return true;
}

static inline checkpoint_block_t*
checkpoint_find(checkpoint_t* state, const void* destination) {
    for (checkpoint_block_t* b = state->first; b != NULL; b = b->next) {
        if (b->owner == NULL && b->destination == destination) {
            return b;
        }
    }
    return NULL;
}

static inline void*
checkpoint_retarget(checkpoint_t* state, const void* source, void* destination) {
    checkpoint_block_t* block = checkpoint_find(state, source);
    if (block == NULL) {
        return NULL;
    }
    block->destination = destination;
    return block->data;
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
