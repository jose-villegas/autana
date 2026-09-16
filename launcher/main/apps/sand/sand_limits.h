#pragma once

#include "gfx/gfx.h"

#define CELL_MIN       2
#define GRID_W_MAX     (GFX_WIDTH / CELL_MIN)
#define GRID_H_MAX     (GFX_HEIGHT / CELL_MIN)
#define BLOCK_COLS_MAX ((GRID_W_MAX + SAND_BLOCK_W - 1) / SAND_BLOCK_W)
#define BLOCK_ROWS_MAX ((GRID_H_MAX + SAND_BLOCK_H - 1) / SAND_BLOCK_H)
