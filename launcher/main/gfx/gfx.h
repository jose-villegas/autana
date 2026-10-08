/*
 * gfx: framebuffer ownership.
 *
 * Everything on this device draws into ONE full-screen RGB565 framebuffer that
 * this module owns. The shell and every app share it; nothing else allocates a
 * buffer of its own. At 368x448x2 it is 322 KiB of PSRAM, and a second one
 * costs a per-frame PSRAM copy no app can afford; see
 * docs/Gfx-and-Presentation.md.
 *
 * Drawing into it is gfx/draw/gfx_draw.h; sending it to the panel is
 * gfx/present/gfx_present.h; the mode it is held in is gfx/present/gfx_mode.h.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "gfx/draw/gfx_color.h"
#include "util/build/build_variant.h"

/* The panel's size as literals, so no caller needs the board's headers; the
 * present side asserts them against the board's own values. */
#define GFX_WIDTH  368
#define GFX_HEIGHT 448

/* The band ring's compile-time band height (gfx_mode.h, gfx_band.h), a
 * divisor of GFX_HEIGHT (448): 64, 32 or 16. 32 is the default absent a
 * device sweep saying otherwise (docs/plans/Autana-Rendering-Roadmap.md section
 * 8, decision 2); override with -DGFX_BAND_HEIGHT=N to try another. */
#ifndef GFX_BAND_HEIGHT
#if defined(CONFIG_LAUNCHER_GFX_BAND_HEIGHT_16) && CONFIG_LAUNCHER_GFX_BAND_HEIGHT_16
#define GFX_BAND_HEIGHT 16
#elif defined(CONFIG_LAUNCHER_GFX_BAND_HEIGHT_64) && CONFIG_LAUNCHER_GFX_BAND_HEIGHT_64
#define GFX_BAND_HEIGHT 64
#else
#define GFX_BAND_HEIGHT 32
#endif
#endif
_Static_assert(GFX_HEIGHT % GFX_BAND_HEIGHT == 0, "GFX_BAND_HEIGHT must divide GFX_HEIGHT evenly");
_Static_assert(GFX_BAND_HEIGHT % 2 == 0, "a band's row range must round to even panel window edges");

/* Brings up the panel and allocates the framebuffer.
 * Returns false if either fails; the reason is logged. */
bool gfx_init(void);

/* Direct access, for renderers that write pixels in bulk (the 3D rasterizer
 * writes here directly rather than going through gfx_pixel per fragment).
 * Whatever is written must be marked with gfx_mark_dirty() (gfx_present.h). */
gfx_color_t* gfx_framebuffer(void);
