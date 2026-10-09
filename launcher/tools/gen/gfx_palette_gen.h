/*
 * gfx_palette_gen, host-only build-time helpers for GFX_LAYOUT_INDEXED: the
 * OKLab colour space every palette generator measures with, and the tables
 * derived from a finished palette (the reverse RGB565 -> index map and the
 * per-(index, Bayer phase) dither tables gfx_indexed.h reads). Choosing the
 * palette's colours is the generator's own work and stays out of this file;
 * nearest entries are found in OKLab so two entries close together do not
 * fight over which colour "belongs" to which by RGB distance alone.
 *
 * Never included from device code: it links libm and runs only on a host.
 */
#pragma once

#include <stdint.h>

#include "gfx/draw/gfx_color.h"
#include "gfx/draw/gfx_palette.h"

/* OKLab, scaled by 100 so a distance reads like a CIE delta E (about 1-2 is a
 * just noticeable difference), from linear-light sRGB. Exported so every
 * palette generator measures colour the same way. */
typedef struct {
    double l, a, b;
} gfx_lab_t;

typedef struct {
    double r, g, b;
} gfx_lin_t;

/* 0xRRGGBB to linear light. */
gfx_lin_t gfx_rgb_to_lin(uint32_t rgb888);

gfx_lab_t gfx_lin_to_lab(gfx_lin_t c);

/* Squared OKLab distance. */
double gfx_lab_dist2(gfx_lab_t p, gfx_lab_t q);

/* Nearest-OKLab index of every one of the 65536 possible native RGB565
 * keys among `palette->entries[first_index .. palette->count)`, a plain
 * flat search with no notion of per-material budgets or groups, the right
 * shape for a standard palette with no such structure of its own. Slow by
 * design (65536 * up to 256 OKLab distances): a generator's own one-time
 * cost, never paid on the device. */
void gfx_palette_gen_build_index_map(const gfx_palette_t* palette, int first_index, uint8_t out_map[65536]);

/* Bakes `palette256`'s own colours against `palette16`'s ordered dither
 * (gfx_dither_covers(), gfx_color.h) into a flat GFX_INDEXED_PALETTE_SIZE *
 * GFX_INDEXED_DITHER16_PHASES table, see gfx_indexed.h's own comment on
 * what that table means and gfx_indexed_expand_row_dither16() on how it is
 * read. `palette16` need not be the palette a 256-colour index map was
 * built from; the two are independent finished palettes here. */
void gfx_palette_gen_build_dither16(const gfx_palette_t* palette256, const gfx_palette_t* palette16,
                                    gfx_color_t out_table[GFX_PALETTE_MAX_ENTRIES * 16]);

/* GFX_DITHER_NONE's own table: a flat 256-entry LUT of each palette256
 * entry's single nearest palette16 colour, never a blend; the same shape
 * gfx_indexed_expand_row() already reads for the 256-colour path. */
void gfx_palette_gen_build_lut_nearest(const gfx_palette_t* palette256, const gfx_palette_t* palette16,
                                       gfx_color_t out_lut[GFX_PALETTE_MAX_ENTRIES]);

/* GFX_DITHER_CELL_CHECKER (`bayer2` false, 2 phases) or
 * GFX_DITHER_CELL_BAYER2's (`bayer2` true, 4) own table;
 * gfx_indexed_expand_row_dither_cell() reads it, one lookup per cell.
 * `out_table` needs GFX_PALETTE_MAX_ENTRIES * (bayer2 ? 4 : 2) entries. */
void gfx_palette_gen_build_dither_cell(const gfx_palette_t* palette256, const gfx_palette_t* palette16, bool bayer2,
                                       gfx_color_t* out_table);

/* GFX_DITHER_PIXEL_CHECKER2's own table;
 * gfx_indexed_expand_row_dither_checker2() reads it, the same shape as
 * gfx_palette_gen_build_dither16() at a 2-pixel period instead of 4x4. */
void gfx_palette_gen_build_dither_checker2(const gfx_palette_t* palette256, const gfx_palette_t* palette16,
                                           gfx_color_t out_table[GFX_PALETTE_MAX_ENTRIES * 2 * 2]);
