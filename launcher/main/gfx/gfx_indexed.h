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

/* One entry per (palette index, Bayer phase): the RGB565 colour that
 * index's dither against the shared 16-colour table resolves to at panel
 * position (x, y), keyed by `index * 16 + (y & 3) * 4 + (x & 3)` - built
 * once, offline (see sand_palette256.h), so expansion never calls
 * gfx_dither_covers() itself. 256 * 16 * 2 bytes = 8 KiB flat. */
#define GFX_INDEXED_DITHER16_PHASES 16

/* Same as gfx_indexed_expand_row(), but every pixel is one lookup into a
 * precomputed (index, phase) table instead of a 256-colour LUT read.
 * `panel_row`/`panel_col0` are the OUTPUT row's absolute panel coordinates -
 * the phase is keyed off them, not a position local to this call, so two
 * dithered bands sent side by side stay in phase (see gfx_dither_covers()'s
 * own comment in gfx_color.h, whose table this one is baked from). */
static inline void
gfx_indexed_expand_row_dither16(const uint8_t* grid_row, int grid_w,
                                const gfx_color_t dither16_rgb[GFX_INDEXED_PALETTE_SIZE * GFX_INDEXED_DITHER16_PHASES],
                                int cell_size, int panel_row, int panel_col0, gfx_color_t* out_row, int out_width) {
    const int grid_pixels = grid_w * cell_size;
    const int solid_pixels = grid_pixels < out_width ? grid_pixels : out_width;
    const int py = (panel_row & 3) * 4;

    for (int x = 0; x < solid_pixels; x++) {
        const int gx = x / cell_size;
        const uint8_t idx = grid_row != NULL ? grid_row[gx] : 0u;
        const int px = (panel_col0 + x) & 3;
        out_row[x] = dither16_rgb[idx * GFX_INDEXED_DITHER16_PHASES + py + px];
    }
    for (int x = solid_pixels; x < out_width; x++) {
        const int px = (panel_col0 + x) & 3;
        out_row[x] = dither16_rgb[py + px]; /* index 0: reserved background */
    }
}

/* Groups the 256 indices by whether `dither16_rgb` renders them IDENTICALLY
 * - same colour at every phase, so a cell whose index moves within a class
 * has provably unchanged output wherever it sits. `out_class[i]` is the
 * smallest index sharing i's own row. O(n^2): call once per install, not
 * per frame - see app_sand.c's paint_row_n() for the change-detection use. */
static inline void
gfx_indexed_dither16_classify(const gfx_color_t dither16_rgb[GFX_INDEXED_PALETTE_SIZE * GFX_INDEXED_DITHER16_PHASES],
                              uint8_t out_class[GFX_INDEXED_PALETTE_SIZE]) {
    for (int i = 0; i < GFX_INDEXED_PALETTE_SIZE; i++) {
        out_class[i] = (uint8_t)i;
        for (int j = 0; j < i; j++) {
            bool same = true;
            for (int p = 0; p < GFX_INDEXED_DITHER16_PHASES; p++) {
                if (dither16_rgb[i * GFX_INDEXED_DITHER16_PHASES + p]
                    != dither16_rgb[j * GFX_INDEXED_DITHER16_PHASES + p]) {
                    same = false;
                    break;
                }
            }
            if (same) {
                out_class[i] = out_class[j];
                break;
            }
        }
    }
}

/* True if a cell moving from `old_idx` to `new_idx` is worth a repaint and
 * a send: always, by raw index, when `dither16_on` is false (256 mode, no
 * further quantizing to exploit); by dither CLASS when it is true (16
 * mode) - see gfx_indexed_dither16_classify()'s own comment for why that
 * is exact, not an approximation. The one decision app_sand.c's
 * paint_row_n() makes per cell under GFX_PIXFMT_INDEXED8. */
static inline bool
gfx_indexed_cell_changed(uint8_t old_idx, uint8_t new_idx, bool dither16_on,
                         const uint8_t dither_class[GFX_INDEXED_PALETTE_SIZE]) {
    if (!dither16_on) {
        return old_idx != new_idx;
    }
    return dither_class[old_idx] != dither_class[new_idx];
}
