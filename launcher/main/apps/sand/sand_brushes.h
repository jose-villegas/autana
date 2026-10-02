/* sand_brushes: the palette's paintable entries, shared by the app, its suites and the browser build. */
#pragma once

#include "material.h"
#include "palette.h"
#include "sand_ui.h"

/* Selected from the palette panel, not cycled - a cycle's cost grows with
 * material count, a panel's doesn't. Only paintable materials get a tile:
 * burning wood is a STATE, not a material (reaction_t.burn_decay). Whole
 * CELLS, not ids: an extended material isn't nameable by id alone (MATX()
 * in material.h). */
static const sand_brush_t sand_brushes[] = {
    SAND_BRUSH_SOLID(CELL_MAKE(MAT_SAND, 0)),  SAND_BRUSH_SOLID(CELL_MAKE(MAT_WATER, 0)),
    SAND_BRUSH_SOLID(CELL_MAKE(MAT_STONE, 0)), SAND_BRUSH_SOLID(CELL_MAKE(MAT_GAS, 0)),
    SAND_BRUSH_SOLID(CELL_MAKE(MAT_FIRE, 0)),  SAND_BRUSH_SOLID(CELL_MAKE(MAT_WOOD, 0)),
    SAND_BRUSH_SOLID(CELL_MAKE(MAT_OIL, 0)),   SAND_BRUSH_SOLID(CELL_MAKE(MAT_LAVA, 0)),
    SAND_BRUSH_SOLID(CELL_MAKE(MAT_ACID, 0)),  SAND_BRUSH_SOLID(CELL_MAKE(MAT_GLASS, 0)),
    SAND_BRUSH_SOLID(CELL_MAKE(MAT_SNOW, 0)),  SAND_BRUSH_SOLID(CELL_MAKE(MAT_DIRT, 0)),
    SAND_BRUSH_SOLID(MATX(MATX_ICE)),          SAND_BRUSH_SPARSE(MATX(MATX_PLANT), SAND_BRUSH_SHARE_PLANT),
    SAND_BRUSH_SOLID(GUNPOWDER_CELL(0)), /* dry, tone 0, see material_brush_color()'s own
                         * comment (material_palette.h) for why the panel tile itself paints a different code */
};
#define SAND_BRUSH_COUNT ((int)(sizeof(sand_brushes) / sizeof(sand_brushes[0])))

_Static_assert(PALETTE_FITS(SAND_BRUSH_COUNT), "the palette panel for SAND_BRUSH_COUNT brushes is taller than the "
                                               "screen at some orientation - see palette_cols()/PALETTE_TILE "
                                               "in palette.h");
