/* Framebuffer ownership. The shell and applications share one RGB565
 * framebuffer; a second full-size buffer would require a per-frame PSRAM
 * copy. Drawing belongs to draw/draw.h, presentation to gfx/present/. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "build/build_variant.h"
#include "gfx/target/gfx_color.h"
#include "gfx/target/gfx_target.h"
#include "math/geometry/recti.h"

/* The panel's size as literals, so no caller needs the board's headers; the
 * present side asserts them against the board's own values. */
#define GFX_WIDTH             368
#define GFX_HEIGHT            448
#define GFX_FRAMEBUFFER_BYTES (GFX_WIDTH * GFX_HEIGHT * 2u)
#define GFX_SCREEN_RECT       ((recti_t){0, 0, GFX_WIDTH, GFX_HEIGHT})

_Static_assert(sizeof(gfx_color_t) == 2, "framebuffer pixels must be two bytes");

/* The band ring's compile-time band height (gfx_mode.h, gfx_band.h), a
 * divisor of GFX_HEIGHT (448): 64, 32 or 16. The default is 32; see
 * docs/gfx/Targets-and-Modes.md. Override with -DGFX_BAND_HEIGHT=N to try
 * another. */
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
 * writes here directly rather than going through draw_pixel per fragment).
 * Whatever is written must be marked with gfx_mark_dirty(). */
gfx_color_t* gfx_framebuffer(void);

/* True while the framebuffer is known to hold `colour` and nothing else: a
 * send filled it (gfx_set_clear_on_send(), gfx_present.h) and nothing has
 * drawn into it or asked for it since. */
bool gfx_framebuffer_holds(gfx_color_t colour);

/* A draw writes into the framebuffer, band or replay strip, with its
 * current clip and dirty-marking policy. */
typedef struct {
    const gfx_target_t* target;
    recti_t clip;
    bool marks;
} gfx_surface_t;

/* Checks present-in-flight, then framebuffer availability. False skips
 * the draw before reading drawing state. */
bool gfx_draw_begin(gfx_surface_t* out);

/* The caller intersects the box with the screen before setting it. */
void gfx_set_clip(recti_t clip);

/* gfx_present() sends only what changed: the panel holds the rest in its own
 * GRAM, and sending is almost the whole cost of a frame. Every draw_* call
 * marks what it touched; code writing through gfx_framebuffer() MUST mark
 * what it wrote, or the screen shows stale, not a crash. */
/* Tracked as a real box per grid cell, so a caller that touched part of a
 * cell may send less than all of it; rows claim whole cells. */
void gfx_mark_dirty(int x, int y, int w, int h);
/* gfx_mark_dirty() for a draw call already inside gfx_draw_begin(), which ran
 * the present guard: one guard per primitive, not two. */
void gfx_mark_drawn(int x, int y, int w, int h);
void gfx_mark_rows_dirty(int y0, int y1);
void gfx_mark_all_dirty(void);
