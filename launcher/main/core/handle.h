/* handle: generational identities tagged by their issuing owner. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define HANDLE_OWNER_BITS       4U
#define HANDLE_SLOT_BITS        12U
#define HANDLE_GENERATION_BITS  16U
#define HANDLE_GENERATION_SHIFT 0U
#define HANDLE_SLOT_SHIFT       HANDLE_GENERATION_BITS
#define HANDLE_OWNER_SHIFT      (HANDLE_SLOT_BITS + HANDLE_GENERATION_BITS)
#define HANDLE_OWNER_MAX        ((1U << HANDLE_OWNER_BITS) - 1U)
#define HANDLE_SLOT_MAX         ((1U << HANDLE_SLOT_BITS) - 1U)
#define HANDLE_GENERATION_MAX   ((1U << HANDLE_GENERATION_BITS) - 1U)

typedef enum { HANDLE_OWNER_TOKEN = 1, HANDLE_OWNER_ASSET_LOAD = 2 } handle_owner_t;

/* Bits: owner 4 | slot 12 | generation 16. */
typedef struct {
    uint32_t id;
} handle_t;

#define HANDLE_NONE ((handle_t){0})

/* Invalid owner, slot or generation returns HANDLE_NONE. */
static inline handle_t
handle_make(handle_owner_t owner, uint32_t slot, uint32_t generation) {
    if ((uint32_t)owner == 0 || (uint32_t)owner > HANDLE_OWNER_MAX || slot > HANDLE_SLOT_MAX || generation == 0
        || generation > HANDLE_GENERATION_MAX) {
        return HANDLE_NONE;
    }
    return (handle_t){((uint32_t)owner << HANDLE_OWNER_SHIFT) | (slot << HANDLE_SLOT_SHIFT) | generation};
}

static inline handle_owner_t
handle_owner(handle_t handle) {
    return (handle_owner_t)(handle.id >> HANDLE_OWNER_SHIFT);
}

static inline uint32_t
handle_slot(handle_t handle) {
    return (handle.id >> HANDLE_SLOT_SHIFT) & HANDLE_SLOT_MAX;
}

static inline uint32_t
handle_generation(handle_t handle) {
    return handle.id & HANDLE_GENERATION_MAX;
}

static inline bool
handle_equal(handle_t a, handle_t b) {
    return a.id == b.id;
}

static inline bool
handle_is_none(handle_t handle) {
    return handle.id == 0;
}

/* False retires the slot permanently: a stale identity must never revive. */
static inline bool
handle_generation_next(uint32_t generation) {
    return generation < HANDLE_GENERATION_MAX;
}
