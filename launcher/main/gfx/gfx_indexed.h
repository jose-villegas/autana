/*
 * gfx_indexed - expanding a row of palette-index bytes into panel pixels,
 * as a standalone, ESP-IDF-free module so a host suite can drive it
 * without a framebuffer or a panel.
 *
 * GFX_PIXFMT_INDEXED8 (gfx_mode.h) keeps an index image in internal RAM
 * instead of an RGB565 band: one byte per grid cell, not per panel pixel.
 * The present task is what turns that back into pixels, once per dirty
 * band, by calling the functions here - a LUT lookup per cell plus a
 * nearest-neighbour upscale by the grid's own cell size, never a per-pixel
 * divide. gfx.c is the only real caller; everything below is pure enough
 * for a host test to call directly.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gfx/gfx_color.h"

#define GFX_INDEXED_PALETTE_SIZE 256

/* The grid row a panel row belongs to, and the reverse: the first/last
 * panel row a grid row occupies. `cell_size` need not divide the panel's
 * own height evenly (app_sand.c's own margin, see its top comment) - these
 * floor/ceil consistently rather than assume it does. */
static inline int
gfx_indexed_panel_row_to_grid_row(int panel_row, int cell_size) {
    return panel_row / cell_size;
}

static inline int
gfx_indexed_grid_row_to_panel_row(int grid_row, int cell_size) {
    return grid_row * cell_size;
}

/* Expands one panel output row from a row of palette-index bytes: pixel x
 * reads `grid_row[x / cell_size]`, looked up in `lut`. `grid_row` may be
 * NULL - a panel row past the grid's own height, the margin app_sand.c's
 * own top comment documents for the RGB565 path - in which case the whole
 * row reads `lut[0]`, the reserved UI/background entry. `out_width` may
 * exceed `grid_w * cell_size`; the remainder past the grid's own width is
 * `lut[0]` too, the same margin on the other axis. */
static inline void
gfx_indexed_expand_row(const uint8_t* grid_row, int grid_w, const gfx_color_t lut[GFX_INDEXED_PALETTE_SIZE],
                       int cell_size, gfx_color_t* out_row, int out_width) {
    const int grid_pixels = grid_w * cell_size;
    const int solid_pixels = grid_pixels < out_width ? grid_pixels : out_width;

    for (int x = 0; x < solid_pixels; x++) {
        const int gx = x / cell_size;
        out_row[x] = grid_row != NULL ? lut[grid_row[gx]] : lut[0];
    }
    for (int x = solid_pixels; x < out_width; x++) {
        out_row[x] = lut[0];
    }
}

/* One 256-entry palette index's dither against a shared 16-colour table:
 * the ordered dither alternates `lo` and `hi` at `alpha`'s own coverage
 * (gfx_dither_covers(), gfx_color.h) - `alpha` 0 always picks `lo`. Built
 * once, offline, by the same palette study that builds the 256-colour LUT
 * (see sand_palette256.h) - never computed per pixel. */
typedef struct {
    uint8_t lo, hi;
    uint8_t alpha;
} gfx_indexed_dither16_t;

/* Same as gfx_indexed_expand_row(), but every pixel is dithered between two
 * of a 16-colour palette instead of read straight from a 256-colour one.
 * `panel_row`/`panel_col0` are the OUTPUT row's absolute panel coordinates -
 * gfx_dither_covers() keys off them, not a position local to this call, so
 * two dithered bands sent side by side stay in phase (see its own comment
 * in gfx_color.h). */
static inline void
gfx_indexed_expand_row_dither16(const uint8_t* grid_row, int grid_w, const gfx_color_t lut16[16],
                                const gfx_indexed_dither16_t table[GFX_INDEXED_PALETTE_SIZE], int cell_size,
                                int panel_row, int panel_col0, gfx_color_t* out_row, int out_width) {
    const int grid_pixels = grid_w * cell_size;
    const int solid_pixels = grid_pixels < out_width ? grid_pixels : out_width;

    for (int x = 0; x < solid_pixels; x++) {
        const int gx = x / cell_size;
        const uint8_t idx = grid_row != NULL ? grid_row[gx] : 0u;
        const gfx_indexed_dither16_t entry = table[idx];
        const bool hi = gfx_dither_covers(panel_col0 + x, panel_row, entry.alpha);
        out_row[x] = lut16[hi ? entry.hi : entry.lo];
    }
    for (int x = solid_pixels; x < out_width; x++) {
        const gfx_indexed_dither16_t entry = table[0];
        const bool hi = gfx_dither_covers(panel_col0 + x, panel_row, entry.alpha);
        out_row[x] = lut16[hi ? entry.hi : entry.lo];
    }
}
