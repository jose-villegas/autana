/* Material swatch shades from material_palette(). The shade depends only
 * on the brush cell and grid position: frame-dependent randomness would
 * defeat the UI command hash and force an idle panel to repaint. */
#pragma once

#include "material.h"
#include "material_palette.h"

/* The cell to paint at (col, row) of an N x N swatch for brush cell `spec`.
 *
 * Gunpowder and the MATX() materials spend their low nibble on STATE, not a
 * shade, so there is no variant axis to sweep - this returns
 * material_brush_color()'s (material_palette.h) own representative cell
 * instead, a flat swatch rather than texture built from bytes that mean
 * something else. */
static inline cell_t
sand_swatch_cell(cell_t spec, int col, int row, int cells) {
    if (cell_is_gunpowder(spec)) {
        return GUNPOWDER_CELL(2);
    }
    if (cell_is_extended(spec)) {
        return spec;
    }

    const material_id_t material = (material_id_t)CELL_MATERIAL(spec);
    const int span = MATERIAL_SHADE_SPAN(material);
    if (span <= 1 || cells <= 0) {
        return CELL_MAKE(material, 0);
    }

    const uint8_t variant = (uint8_t)(material_grain_hash(col, row) % (unsigned)span);
    return CELL_MAKE(material, variant);
}
