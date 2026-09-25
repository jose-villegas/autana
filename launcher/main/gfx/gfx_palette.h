/*
 * gfx_palette - what a palette IS, for GFX_PIXFMT_INDEXED8: a named list of
 * RGB565 entries, and the lookup a caller does against a reverse map built
 * for it. Standalone and ESP-IDF-free, like gfx_indexed.h.
 *
 * Building a palette (which colours it holds) and building a reverse map or
 * dither table FOR one are host-only, offline work - see
 * tools/gen/gfx_palette_gen.h. This header is only what a caller reads at
 * runtime: the data shape, the UI-reserved convention, and the one lookup
 * a colour-to-index step needs.
 */
#pragma once

#include <stdint.h>

#include "gfx/gfx_color.h"

/* Indices 0-15 are reserved for UI (the shell's own chrome, drawn through
 * whichever palette an app installed) in any palette meant for
 * GFX_PIXFMT_INDEXED8 - a shading colour has no business landing on one. */
#define GFX_PALETTE_UI_ENTRIES  16

#define GFX_PALETTE_MAX_ENTRIES 256

typedef struct {
    const char* name;
    const gfx_color_t* entries;
    int count; /* <= GFX_PALETTE_MAX_ENTRIES; 16 for a 16-colour palette */
} gfx_palette_t;

/* Nearest-entry index of `c` in whichever palette `index_map` was built
 * for (tools/gen/gfx_palette_gen.h's gfx_palette_gen_build_index_map()) - one
 * flash read, keyed by native (non-byte-swapped) RGB565, the same swap
 * gfx_color_rgb888() undoes. */
static inline int
gfx_palette_index_of(gfx_color_t c, const uint8_t index_map[65536]) {
    const uint16_t native = (uint16_t)((c >> 8) | (c << 8));
    return index_map[native];
}
