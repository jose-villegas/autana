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

/* Nearest-entry index of `c` in whichever palette `index_map` was built
 * for (tools/gen/draw_palette_gen.h's draw_palette_gen_build_index_map()); one
 * flash read, keyed by native (non-byte-swapped) RGB565, the same swap
 * rgb565be_to_rgb888() undoes. */
static inline int
palette_index_of(rgb565be_t c, const uint8_t index_map[65536]) {
    return index_map[rgb565_swap(c)];
}
