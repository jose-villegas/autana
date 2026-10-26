/* palette: named RGB565 entries and lookup through a caller-owned reverse map. */
#pragma once

#include <stdint.h>

#include "image/pixel/rgb565.h"

#define PALETTE_MAX_ENTRIES 256

typedef struct {
    const char* name;
    const rgb565be_t* entries;
    int count; /* <= PALETTE_MAX_ENTRIES; 16 for a 16-colour palette */
} palette_t;

/* index_map is the 64 KiB reverse map from native RGB565 to index,
 * built for the palette offline. */
static inline int
palette_index_of(rgb565be_t c, const uint8_t index_map[65536]) {
    return index_map[rgb565_swap(c)];
}
