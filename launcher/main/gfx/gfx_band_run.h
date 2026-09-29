/*
 * gfx_band_run - one transient-picture frame. gfx owns the band ring and
 * send; callers supply the picture and optional overlay rows. The target
 * holds [row0, row1) at full width. Outside GFX_LAYOUT_BANDS it does nothing.
 */
#pragma once

#include <stdbool.h>

#include "gfx/gfx_color.h"

typedef void (*gfx_band_draw_fn)(int row0, int row1, gfx_color_t* target);
typedef void (*gfx_band_overlay_fn)(int row0, int row1);

void gfx_band_run(gfx_band_draw_fn draw, gfx_band_overlay_fn overlay);
