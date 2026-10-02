/* sand_dither_tables: which generated sand_palette256.h table each 16-colour dither mode expands through. */
#pragma once

#include "gfx/gfx_indexed.h"
#include "sand_palette256.h"

static inline const gfx_color_t*
sand_dither_table_for(gfx_dither_mode_t mode) {
    switch (mode) {
        case GFX_DITHER_NONE: return sand_dither_none_lut;
        case GFX_DITHER_CELL_CHECKER: return sand_dither_cell_checker;
        case GFX_DITHER_CELL_BAYER2: return sand_dither_cell_bayer2;
        case GFX_DITHER_PIXEL_CHECKER2: return sand_dither_pixel_checker2;
        case GFX_DITHER_PIXEL_BAYER4:
        default: return sand_palette16_dither_rgb;
    }
}
