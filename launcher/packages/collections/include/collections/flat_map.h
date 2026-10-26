/* flat_map: rows found by a 32-bit key the rows carry, kept in ascending key order by whoever wrote them, so a find
 * is a binary search where the rows already sit and nothing is built or copied. Keys are little-endian at the same
 * offset in every row; opening checks they strictly ascend, which also refuses two rows sharing a key. */
#pragma once

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#include "collections/bytes.h"

typedef struct {
    const uint8_t* rows;
    size_t count;
    size_t stride;     /* bytes from one row to the next */
    size_t key_offset; /* bytes from a row's start to its key */
} flat_map_t;

typedef enum {
    FLAT_MAP_OK,
    FLAT_MAP_UNSORTED,  /* a key is below the one before it */
    FLAT_MAP_DUPLICATE, /* a key equals the one before it */
} flat_map_status_t;

#define FLAT_MAP_MISSING SIZE_MAX

static inline const uint8_t*
flat_map_row(const flat_map_t* map, size_t index) {
    return map->rows + (index * map->stride);
}

static inline uint32_t
flat_map_key(const flat_map_t* map, size_t index) {
    return bytes_read_u32(flat_map_row(map, index) + map->key_offset);
}

/* Opens count rows at rows. On a refusal *map is left empty and *clash, when given, is the first row out of order. */
static inline flat_map_status_t
flat_map_open(flat_map_t* map, const void* rows, size_t count, size_t stride, size_t key_offset, size_t* clash) {
    assert(key_offset + sizeof(uint32_t) <= stride);
    *map = (flat_map_t){rows, count, stride, key_offset};
    for (size_t i = 1; i < count; i++) {
        const uint32_t before = flat_map_key(map, i - 1);
        const uint32_t key = flat_map_key(map, i);
        if (key <= before) {
            *map = (flat_map_t){0};
            if (clash != NULL) {
                *clash = i;
            }
            return key == before ? FLAT_MAP_DUPLICATE : FLAT_MAP_UNSORTED;
        }
    }
    return FLAT_MAP_OK;
}

/* The row holding key, or FLAT_MAP_MISSING. A key derived from a name may still be another name's: the caller
 * confirms the row's name. */
static inline size_t
flat_map_find(const flat_map_t* map, uint32_t key) {
    size_t low = 0;
    size_t high = map->count;
    while (low < high) {
        const size_t mid = low + ((high - low) / 2);
        const uint32_t at = flat_map_key(map, mid);
        if (at == key) {
            return mid;
        }
        if (at < key) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }
    return FLAT_MAP_MISSING;
}
