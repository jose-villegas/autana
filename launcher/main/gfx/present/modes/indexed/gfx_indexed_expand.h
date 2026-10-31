/*
 * gfx_indexed_expand: expanding a row of palette-index bytes into panel
 * pixels, for the present task (gfx_present.c) and readback (gfx_mode.c),
 * with the state they read in gfx_indexed.c. A LUT lookup per cell plus a
 * nearest-neighbour upscale by the grid's own cell size, never a per-pixel
 * divide. ESP-IDF-free, so a host suite drives it directly.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gfx/present/modes/indexed/gfx_indexed.h"

/* The grid row a panel row belongs to, and the reverse: the first/last
 * panel row a grid row occupies. `cell_size` need not divide the panel's
 * own height evenly (a caller may reserve a margin below the grid's own
 * rows), so these floor/ceil consistently rather than assume it does. */
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
 * NULL (a panel row past the grid's own height, such as a margin a caller
 * reserves below its grid), in which case the whole row reads `lut[0]`,
 * the reserved UI/background entry. `out_width` may exceed
 * `grid_w * cell_size`; the remainder past the grid's own width is
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

/* Same as gfx_indexed_expand_row(), but every pixel is one lookup into a
 * precomputed (index, phase) table instead of a 256-colour LUT read.
 * `panel_row`/`panel_col0` are the OUTPUT row's absolute panel coordinates:
 * the phase is keyed off them, not a position local to this call, so two
 * dithered bands sent side by side stay in phase (see bayer_covers()'s
 * own comment in bayer.h, whose table this one is baked from). */
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

/* One lookup per CELL, keyed by its own (cx, cy) phase, filled solid
 * across its pixels by a run fill; `bayer2` picks the phase formula
 * matching whichever table `cell_table` was baked for, resolved once per
 * cell rather than the divide, ternary and multiply gfx_indexed_expand_row()
 * never needs repeated once per PANEL PIXEL. `cy` is the GRID row: a cell's
 * phase never depends on which panel pixel is being written. */
static inline void
gfx_indexed_expand_row_dither_cell(const uint8_t* grid_row, int grid_w, const gfx_color_t* cell_table, bool bayer2,
                                   int cell_size, int cy, gfx_color_t* out_row, int out_width) {
    const int phases = bayer2 ? GFX_INDEXED_CELL_BAYER2_PHASES : GFX_INDEXED_CELL_CHECKER_PHASES;
    const int cy_term = bayer2 ? (cy & 1) * 2 : (cy & 1);
    int x = 0;
    for (int gx = 0; x < out_width; gx++) {
        const uint8_t idx = (grid_row != NULL && gx < grid_w) ? grid_row[gx] : 0u;
        const int phase = bayer2 ? (cy_term + (gx & 1)) : ((cy_term + gx) & 1);
        const gfx_color_t colour = cell_table[idx * phases + phase];
        const int run_end = x + cell_size < out_width ? x + cell_size : out_width;
        for (; x < run_end; x++) {
            out_row[x] = colour;
        }
    }
}

/* Same shape as gfx_indexed_expand_row_dither16(), read at a 2-pixel
 * period instead of 4: `panel_row`/`panel_col0` are absolute panel
 * coordinates, so two dithered bands sent side by side stay in phase. */
static inline void
gfx_indexed_expand_row_dither_checker2(
    const uint8_t* grid_row, int grid_w,
    const gfx_color_t
        checker2_rgb[GFX_INDEXED_PALETTE_SIZE * GFX_INDEXED_CHECKER2_ROW_PHASES * GFX_INDEXED_CHECKER2_CHUNK_PX],
    int cell_size, int panel_row, int panel_col0, gfx_color_t* out_row, int out_width) {
    const int stride = GFX_INDEXED_CHECKER2_ROW_PHASES * GFX_INDEXED_CHECKER2_CHUNK_PX;
    const int grid_pixels = grid_w * cell_size;
    const int solid_pixels = grid_pixels < out_width ? grid_pixels : out_width;
    const int py = (panel_row & 1) * GFX_INDEXED_CHECKER2_CHUNK_PX;

    for (int x = 0; x < solid_pixels; x++) {
        const int gx = x / cell_size;
        const uint8_t idx = grid_row != NULL ? grid_row[gx] : 0u;
        const int px = (panel_col0 + x) & 1;
        out_row[x] = checker2_rgb[idx * stride + py + px];
    }
    for (int x = solid_pixels; x < out_width; x++) {
        const int px = (panel_col0 + x) & 1;
        out_row[x] = checker2_rgb[py + px]; /* index 0: reserved background */
    }
}

/* Everything that decides how an index image reaches the panel. `table` is
 * the plain 256-colour LUT while `dither16_on` is false, otherwise the one
 * installed for `dither_mode`, sized as that mode's own comment says. */
typedef struct {
    const uint8_t* image;
    int grid_w, grid_h, cell_size;
    bool dither16_on;
    gfx_dither_mode_t dither_mode;
    const gfx_color_t* table;
} gfx_indexed_frame_t;

/* Panel row `y` of `frame`, exactly as the present path sends it. The one
 * place the per-mode choice is made, so a read-back of the panel cannot
 * drift from what was sent. */
static inline void
gfx_indexed_expand_panel_row(const gfx_indexed_frame_t* frame, int y, gfx_color_t* out_row, int out_width) {
    const int grid_row = gfx_indexed_panel_row_to_grid_row(y, frame->cell_size);
    const uint8_t* row = grid_row < frame->grid_h ? frame->image + (size_t)grid_row * frame->grid_w : NULL;
    const int w = frame->grid_w;
    const int cell = frame->cell_size;

    if (!frame->dither16_on) {
        gfx_indexed_expand_row(row, w, frame->table, cell, out_row, out_width);
        return;
    }
    switch (frame->dither_mode) {
        case GFX_DITHER_CELL_CHECKER:
            gfx_indexed_expand_row_dither_cell(row, w, frame->table, false, cell, grid_row, out_row, out_width);
            return;
        case GFX_DITHER_CELL_BAYER2:
            gfx_indexed_expand_row_dither_cell(row, w, frame->table, true, cell, grid_row, out_row, out_width);
            return;
        case GFX_DITHER_PIXEL_CHECKER2:
            gfx_indexed_expand_row_dither_checker2(row, w, frame->table, cell, y, 0, out_row, out_width);
            return;
        case GFX_DITHER_NONE: gfx_indexed_expand_row(row, w, frame->table, cell, out_row, out_width); return;
        case GFX_DITHER_PIXEL_BAYER4:
        case GFX_DITHER_MODE_COUNT:
        default: gfx_indexed_expand_row_dither16(row, w, frame->table, cell, y, 0, out_row, out_width); return;
    }
}
