/*
 * gfx_palette_gen - host-only build-time helpers for GFX_PIXFMT_INDEXED8:
 * given a finished palette, build the reverse RGB565 -> index map an
 * app's own material_palette256_index()-style lookup needs, or the
 * per-(index, Bayer phase) dither table gfx_indexed_expand_row_dither16()
 * needs. Building the PALETTE ITSELF - which colours it holds, and by what
 * weighting - is app-specific work (see main/apps/sand/tools/
 * shading_palette.c's own k-means budget allocation) and stays out of this
 * file; this is only the two derived-table steps every such generator
 * needs afterward, done once in OKLab so two entries near each other in a
 * palette do not fight over which colour "belongs" to which by RGB
 * distance alone.
 *
 * Never included from device code - it links libm and is only ever run on
 * a host, the same reason main/apps/sand/tools/ itself is excluded from
 * the firmware image (see docs/Building-an-App.md, "An app is a
 * folder").
 */
#pragma once

#include <stdint.h>

#include "gfx/gfx_color.h"
#include "gfx/gfx_palette.h"

/* Nearest-OKLab index of every one of the 65536 possible native RGB565
 * keys among `palette->entries[first_index .. palette->count)` - a plain
 * flat search with no notion of per-material budgets or groups, the right
 * shape for a standard palette with no such structure of its own. Slow by
 * design (65536 * up to 256 OKLab distances): a generator's own one-time
 * cost, never paid on the device. */
void gfx_palette_gen_build_index_map(const gfx_palette_t* palette, int first_index, uint8_t out_map[65536]);

/* Bakes `palette256`'s own colours against `palette16`'s ordered dither
 * (gfx_dither_covers(), gfx_color.h) into a flat GFX_INDEXED_PALETTE_SIZE *
 * GFX_INDEXED_DITHER16_PHASES table - see gfx_indexed.h's own comment on
 * what that table means and gfx_indexed_expand_row_dither16() on how it is
 * read. `palette16` need not be the palette a 256-colour index map was
 * built from; the two are independent finished palettes here. */
void gfx_palette_gen_build_dither16(const gfx_palette_t* palette256, const gfx_palette_t* palette16,
                                    gfx_color_t out_table[GFX_PALETTE_MAX_ENTRIES * 16]);

/* GFX_DITHER_NONE's own table: a flat 256-entry LUT of each palette256
 * entry's single nearest palette16 colour, never a blend - the same shape
 * gfx_indexed_expand_row() already reads for the 256-colour path. */
void gfx_palette_gen_build_lut_nearest(const gfx_palette_t* palette256, const gfx_palette_t* palette16,
                                       gfx_color_t out_lut[GFX_PALETTE_MAX_ENTRIES]);

/* GFX_DITHER_CELL_CHECKER (`bayer2` false, 2 phases) or
 * GFX_DITHER_CELL_BAYER2's (`bayer2` true, 4) own table -
 * gfx_indexed_expand_row_dither_cell() reads it, one lookup per cell.
 * `out_table` needs GFX_PALETTE_MAX_ENTRIES * (bayer2 ? 4 : 2) entries. */
void gfx_palette_gen_build_dither_cell(const gfx_palette_t* palette256, const gfx_palette_t* palette16, bool bayer2,
                                       gfx_color_t* out_table);

/* GFX_DITHER_PIXEL_CHECKER2's own table -
 * gfx_indexed_expand_row_dither_checker2() reads it, the same shape as
 * gfx_palette_gen_build_dither16() at a 2-pixel period instead of 4x4. */
void gfx_palette_gen_build_dither_checker2(const gfx_palette_t* palette256, const gfx_palette_t* palette16,
                                           gfx_color_t out_table[GFX_PALETTE_MAX_ENTRIES * 2 * 2]);
