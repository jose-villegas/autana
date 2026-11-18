/*
 * gfx_present: getting the framebuffer (gfx.h) onto the panel. What
 * changed is marked, and only that is sent, over a QSPI link whose clock
 * an app may choose.
 */
#pragma once

#include "build/build_variant.h"
#if BUILD_VARIANT_DEVELOPMENT_OR_HOST
typedef struct checkpoint checkpoint_t;
void gfx_checkpoint_capture(checkpoint_t* saved);
#endif

#include <stdbool.h>

#include "gfx/gfx.h"
#include "gfx/target/gfx_target.h"

/* The band ring's compile-time band height (gfx.h's gfx_mode_t; gfx_band.h), a
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
#ifndef ESP_PLATFORM
/* Gives back what gfx_init() allocated, so a host run can open again in one process. */
void gfx_deinit(void);
#endif

/* True while the framebuffer is known to hold `colour` and nothing else: a
 * send filled it (gfx_mode_request_t's clear_on_send) and nothing has drawn into it or
 * asked for it since. */
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

/* gfx_mark_dirty() for a draw call already inside gfx_draw_begin(), which ran
 * the present guard: one guard per primitive, not two. Rows claim whole
 * cells. */
void gfx_mark_drawn(int x, int y, int w, int h);
void gfx_mark_rows_dirty(int y0, int y1);

/* QSPI clock for the panel: 16.5 ms of bus a full frame at 40 MHz, 8.2 at
 * 80. 80 needs every strip sent from internal DMA RAM (strip_bounce),
 * exceeds the panel's rated 50 MHz, so every send at it is full-width
 * rows; see
 * CONFIG_LAUNCHER_GFX_QSPI_80MHZ. The divider resolves to exactly 40 or 80,
 * hence a bool. The thresholds in gfx_dirty.h are fitted to 40 MHz. */
#if defined(CONFIG_LAUNCHER_GFX_QSPI_80MHZ) && CONFIG_LAUNCHER_GFX_QSPI_80MHZ
#define GFX_QSPI_HZ (80 * 1000 * 1000)
#else
#define GFX_QSPI_HZ (40 * 1000 * 1000)
#endif

/* Enable or disable interlace mode. When enabled, gfx_present() updates
 * only even-numbered strips on even frames and odd-numbered strips on odd
 * frames. Off by default. */
void gfx_set_interlace(bool enabled);
bool gfx_interlace_enabled(void);

/* True from a gfx_request_full_redraw() call until the shell clears it for
 * the pass that follows. */
bool gfx_full_redraw_pending(void);

/* Ends the window gfx_request_full_redraw() opened, called by the shell
 * once it has read the flag and decided whether to invoke an app's
 * invalidate(), before that pass's frame() runs. */
void gfx_full_redraw_clear_pending(void);

/* Whether any recorded dirt overlaps this rectangle. The tracker uses
 * leaf-sized regions for precise marks and full cells for band marks. */
bool gfx_region_dirty(int x, int y, int w, int h);

/* The dirt tracker's grid: bands GFX_DIRTY_STRIP_HEIGHT rows tall, each
 * split into GFX_DIRTY_LEAF_SUB rows of leaves (send/gfx_dirty.h). */
#define GFX_DIRTY_STRIP_HEIGHT 64
#define GFX_DIRTY_LEAF_SUB     4
#define GFX_DIRTY_LEAF_ROWS    (GFX_HEIGHT / GFX_DIRTY_STRIP_HEIGHT * GFX_DIRTY_LEAF_SUB)

/* The dirt recorded so far this frame, kept apart from whatever is drawn
 * after: one bit a leaf, a word a leaf row. */
typedef struct {
    uint16_t rows[GFX_DIRTY_LEAF_ROWS];
} gfx_dirty_leaves_t;

void gfx_dirty_leaves(gfx_dirty_leaves_t* out);

/* Adds to `to` the leaves of `from` that `area` touches. */
void gfx_dirty_leaves_add(gfx_dirty_leaves_t* to, const gfx_dirty_leaves_t* from, recti_t area);

/* Takes one rect of `left`'s leaves inside `area` off it, clipped to
 * `area`; false once none is left there. The rects taken never overlap
 * and together cover every leaf `area` touches. */
bool gfx_dirty_leaves_take(gfx_dirty_leaves_t* left, recti_t area, recti_t* out);

/* Exactly gfx_present_begin() followed by gfx_present_wait(). In
 * GFX_LAYOUT_DOUBLE_FB this drains the previous send, swaps front and back,
 * and dispatches every strip; wait returns immediately so drawing can
 * overlap the send. Every frame must redraw every pixel. Mode exit and
 * readback drain the last send. Other layouts wait for their transfers. */
void gfx_present(void);

/*
 * Split present: gfx_present_begin() hands the framebuffer to the core-1
 * present task and returns at once; in double mode it first drains the
 * previous send. gfx_present_wait() blocks until sent, except in double mode.
 * Outside double mode, no gfx_* call that touches drawing state or the
 * framebuffer may run between begin and wait: the update() contract,
 * asserted in development builds (gfx_present_guard.h).
 * gfx_set_present_async(false) sends synchronously on the caller for A/B measurement.
 */
void gfx_present_begin(void);
void gfx_present_wait(void);

void gfx_set_present_async(bool on);
bool gfx_present_async_enabled(void);

/* The panel link's clock. The fast rate halves bus time but is past the
 * panel's rated 50 MHz: there the panel sometimes lands a partial-width
 * window a column off, so gfx sends only full-width rows at it. gfx starts
 * at GFX_QSPI_HZ and keeps whatever it was last told; it does not choose
 * or remember a rate itself. */
#define GFX_PANEL_CLOCK_SLOW_HZ (40 * 1000 * 1000)
#define GFX_PANEL_CLOCK_FAST_HZ (80 * 1000 * 1000)

/* Takes effect before the next present sends anything, never mid-send.
 * Returns false, changing nothing, for any other rate. */
bool gfx_set_panel_clock_hz(int hz);
int gfx_panel_clock_hz(void);
