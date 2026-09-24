/*
 * sand_mode_swatches - what each colour mode's tile on the options screen
 * shows: its real colours, not an icon standing for them. 16 is the exact
 * sixteen the mode reduces to, 256 a sample of the sand palette, FULL a
 * smooth hue sweep, since it has no palette to sample.
 *
 * Pure: the palettes come in as arguments, so the app, a host render and
 * suite_sand_mode_swatches.c all build them the same way.
 */
#pragma once

#include <stdint.h>

#include "apps/sand/sand_colour_state.h"
#include "gfx/gfx_color.h"

/* Two rows across a tile's width, where a wide strip reads better than a
 * square at 32px tall. */
#define SAND_SWATCH_ROWS       2
#define SAND_SWATCH_16_COLS    8
#define SAND_SWATCH_256_COLS   12
#define SAND_SWATCH_FULL_BANDS 32
#define SAND_SWATCH_MAX        32

typedef struct {
    uint32_t rgb[SAND_SWATCH_MAX]; /* 0xRRGGBB, row by row */
    int cols;
    int rows;
} sand_mode_swatch_t;

/* Fills `out`, indexed by sand_colour_mode_t. `none_lut` maps every index
 * to one of the 16-colour mode's colours; the first `ui_entries` of
 * `lut256` are UI colours, not sand, and are left out of its sample. */
void sand_mode_swatches(const gfx_color_t* none_lut, const gfx_color_t* lut256, int lut_size, int ui_entries,
                        sand_mode_swatch_t out[3]);
