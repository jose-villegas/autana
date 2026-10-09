/* sand_brushes: the palette's paintable entries, shared by the app and its suites. */
#pragma once

#include "material.h"
#include "palette.h"
#include "sand_ui.h"

/* Selected from the palette panel, not cycled - a cycle's cost grows with
 * material count, a panel's doesn't. Only paintable materials get a tile:
 * burning wood is a STATE, not a material (reaction_t.burn_decay). Whole
 * CELLS, not ids: an extended material isn't nameable by id alone (MATX()
 * in material.h). */
#define SAND_BRUSH_MAT(m) SAND_BRUSH_SOLID(CELL_MAKE((m), 0))

static const sand_brush_t sand_brushes[] = {
    SAND_BRUSH_MAT(MAT_SAND),
    SAND_BRUSH_MAT(MAT_WATER),
    SAND_BRUSH_MAT(MAT_STONE),
    SAND_BRUSH_MAT(MAT_GAS),
    SAND_BRUSH_MAT(MAT_FIRE),
    SAND_BRUSH_MAT(MAT_WOOD),
    SAND_BRUSH_MAT(MAT_OIL),
    SAND_BRUSH_MAT(MAT_LAVA),
    SAND_BRUSH_MAT(MAT_ACID),
    SAND_BRUSH_MAT(MAT_GLASS),
    SAND_BRUSH_MAT(MAT_SNOW),
    SAND_BRUSH_MAT(MAT_DIRT),
    SAND_BRUSH_SOLID(MATX(MATX_ICE)),
    SAND_BRUSH_SPARSE(MATX(MATX_PLANT), SAND_BRUSH_SHARE_PLANT),
    SAND_BRUSH_SOLID(GUNPOWDER_CELL(0)), /* dry, tone 0, see material_brush_color()'s own
                         * comment (material_palette.h) for why the panel tile itself paints a different code */
};
#define SAND_BRUSH_COUNT ((int)(sizeof(sand_brushes) / sizeof(sand_brushes[0])))

_Static_assert(PALETTE_FITS(SAND_BRUSH_COUNT), "the palette panel for SAND_BRUSH_COUNT brushes is taller than the "
                                               "screen at some orientation - see palette_cols()/PALETTE_TILE "
                                               "in palette.h");
