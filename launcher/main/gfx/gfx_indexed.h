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
 * own height evenly - a caller may reserve a margin below the grid's own
 * rows - these floor/ceil consistently rather than assume it does. */
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
 * NULL - a panel row past the grid's own height, such as a margin a caller
 * reserves below its grid - in which case the whole row reads `lut[0]`,
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

/* One entry per (palette index, Bayer phase): the RGB565 colour that
 * index's dither against the shared 16-colour table resolves to at panel
 * position (x, y), keyed by `index * 16 + (y & 3) * 4 + (x & 3)` - built
 * once offline, so expansion never calls gfx_dither_covers() itself.
 * 256 * 16 * 2 bytes = 8 KiB flat. */
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

/* Groups the 256 indices by whether a `phases`-wide table renders them
 * IDENTICALLY - a cell whose index moves within a class has provably
 * unchanged output wherever it sits. `out_class[i]` is the smallest index
 * sharing i's own row. The PIXEL modes' own rule; gfx_indexed_cell_
 * dither_changed() is the CELL modes' tighter one. */
static inline void
gfx_indexed_classify(const gfx_color_t* table, int phases, uint8_t out_class[GFX_INDEXED_PALETTE_SIZE]) {
    for (int i = 0; i < GFX_INDEXED_PALETTE_SIZE; i++) {
        out_class[i] = (uint8_t)i;
        for (int j = 0; j < i; j++) {
            bool same = true;
            for (int p = 0; p < phases; p++) {
                if (table[i * phases + p] != table[j * phases + p]) {
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

/* gfx_indexed_classify() at GFX_DITHER_PIXEL_BAYER4's own width - kept
 * under its historical name for every existing caller. */
static inline void
gfx_indexed_dither16_classify(const gfx_color_t dither16_rgb[GFX_INDEXED_PALETTE_SIZE * GFX_INDEXED_DITHER16_PHASES],
                              uint8_t out_class[GFX_INDEXED_PALETTE_SIZE]) {
    gfx_indexed_classify(dither16_rgb, GFX_INDEXED_DITHER16_PHASES, out_class);
}

/* True if a cell moving from `old_idx` to `new_idx` is worth a repaint and
 * a send: always, by raw index, when `dither16_on` is false (256 mode, no
 * further quantizing to exploit); by dither CLASS when it is true (16
 * mode) - see gfx_indexed_dither16_classify()'s own comment for why that
 * is exact, not an approximation. The one decision an indexed-mode row
 * painter makes per cell under GFX_PIXFMT_INDEXED8. */
static inline bool
gfx_indexed_cell_changed(uint8_t old_idx, uint8_t new_idx, bool dither16_on,
                         const uint8_t dither_class[GFX_INDEXED_PALETTE_SIZE]) {
    if (!dither16_on) {
        return old_idx != new_idx;
    }
    return dither_class[old_idx] != dither_class[new_idx];
}

/* gfx_indexed_cell_changed(), plus the one case it cannot see for itself:
 * `force_full` set means the PANEL, not just this cell's own index, needs
 * repainting - an overlay just closed, the board turned - so every visited
 * cell is worth a write and a send whether or not its index moved. Never
 * narrow that with the change compare above; only ever widen past it. */
static inline bool
gfx_indexed_cell_needs_repaint(bool force_full, uint8_t old_idx, uint8_t new_idx, bool dither16_on,
                               const uint8_t dither_class[GFX_INDEXED_PALETTE_SIZE]) {
    return force_full || gfx_indexed_cell_changed(old_idx, new_idx, dither16_on, dither_class);
}

/* Lever 2: which SPATIAL PATTERN a 16-colour reduction paints with - the
 * blend choice itself (tools/gfx_palette_gen.h) is shared by all five;
 * only where lo/hi land differs, solid cells to per-pixel dither. */
typedef enum {
    GFX_DITHER_NONE,           /* nearest of the 16, no blend - one solid colour per cell */
    GFX_DITHER_CELL_CHECKER,   /* solid, alternating by (cx + cy) & 1 */
    GFX_DITHER_CELL_BAYER2,    /* solid, a 2x2 Bayer threshold over cells */
    GFX_DITHER_PIXEL_CHECKER2, /* a 2x2 ordered dither, per panel pixel */
    GFX_DITHER_PIXEL_BAYER4,   /* today's 4x4 ordered dither, per panel pixel */
    GFX_DITHER_MODE_COUNT,
} gfx_dither_mode_t;

/* GFX_DITHER_CELL_CHECKER's own table: 2 phases (cx+cy even/odd) per
 * index, each one solid colour - same cell-fill cost as
 * gfx_indexed_expand_row()'s own 256-colour LUT path, one lookup per cell,
 * never per pixel. */
#define GFX_INDEXED_CELL_CHECKER_PHASES 2

/* GFX_DITHER_CELL_BAYER2's own table: 4 phases, (cy & 1) * 2 + (cx & 1) -
 * the four positions of a 2x2 Bayer block over cells, not pixels. */
#define GFX_INDEXED_CELL_BAYER2_PHASES  4

/* One lookup per CELL, keyed by its own (cx, cy) phase, filled solid
 * across its pixels by a run fill - `bayer2` picks the phase formula
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

/* Exact, not an approximation: a fixed cell never leaves its own (cx, cy)
 * phase, so comparing the two indices' resolved colours at THAT phase
 * alone is the whole answer - unlike gfx_indexed_dither16_classify()'s
 * every-phase match, needed only because a PIXEL mode's cell spans several. */
static inline bool
gfx_indexed_cell_dither_changed(uint8_t old_idx, uint8_t new_idx, const gfx_color_t* cell_table, bool bayer2, int cx,
                                int cy) {
    if (old_idx == new_idx) {
        return false;
    }
    const int phases = bayer2 ? GFX_INDEXED_CELL_BAYER2_PHASES : GFX_INDEXED_CELL_CHECKER_PHASES;
    const int phase = bayer2 ? ((cy & 1) * 2 + (cx & 1)) : ((cx + cy) & 1);
    return cell_table[(int)old_idx * phases + phase] != cell_table[(int)new_idx * phases + phase];
}

/* Which rule an indexed-mode row painter's hot loop resolves to, decided
 * once per indexed-mode entry - never per cell, and never by a runtime
 * dither16_on/dither_mode pair re-examined on every visit the way each
 * hand-written caller once did. RAW is 256
 * mode; CLASS covers NONE and the two PIXEL modes, all a single 256-entry
 * lookup; the two CELL kinds carry their own phase formula and never touch
 * a class table at all. */
typedef enum {
    GFX_INDEXED_REPAINT_RAW,
    GFX_INDEXED_REPAINT_CLASS,
    GFX_INDEXED_REPAINT_CELL_CHECKER,
    GFX_INDEXED_REPAINT_CELL_BAYER2,
} gfx_indexed_repaint_kind_t;

/* The one per-cell decision, cheap in every kind: `force_full` widens
 * before either table is touched, and an unmoved index answers `false`
 * before any lookup at all - guards a switch-per-cell shape once skipped.
 * `class_table` is read for GFX_INDEXED_REPAINT_CLASS, `cell_table` for the
 * two CELL kinds; the kind picks which one, so the unused pointer may be
 * NULL. */
static inline bool
gfx_indexed_cell_repaint(gfx_indexed_repaint_kind_t kind, const uint8_t* class_table, const gfx_color_t* cell_table,
                         bool force_full, uint8_t old_idx, uint8_t new_idx, int cx, int cy) {
    if (force_full) {
        return true;
    }
    if (old_idx == new_idx) {
        return false;
    }
    switch (kind) {
        case GFX_INDEXED_REPAINT_RAW: return true;
        case GFX_INDEXED_REPAINT_CLASS: return class_table[old_idx] != class_table[new_idx];
        case GFX_INDEXED_REPAINT_CELL_CHECKER: {
            const int phase = (cx + cy) & 1;
            return cell_table[(int)old_idx * GFX_INDEXED_CELL_CHECKER_PHASES + phase]
                   != cell_table[(int)new_idx * GFX_INDEXED_CELL_CHECKER_PHASES + phase];
        }
        case GFX_INDEXED_REPAINT_CELL_BAYER2: {
            const int phase = (cy & 1) * 2 + (cx & 1);
            return cell_table[(int)old_idx * GFX_INDEXED_CELL_BAYER2_PHASES + phase]
                   != cell_table[(int)new_idx * GFX_INDEXED_CELL_BAYER2_PHASES + phase];
        }
    }
    return true;
}

/* GFX_DITHER_PIXEL_CHECKER2's own table: like GFX_INDEXED_DITHER16_PHASES
 * but at half the period - one 2-pixel/4-byte chunk per (index, row
 * phase 0-1); GFX_DITHER_PIXEL_BAYER4 keeps the finer 4x4 pattern. */
#define GFX_INDEXED_CHECKER2_ROW_PHASES 2
#define GFX_INDEXED_CHECKER2_CHUNK_PX   2

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
