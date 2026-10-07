/*
 * gfx_present: getting the framebuffer (gfx.h) onto the panel. What
 * changed is marked, and only that is sent, over a QSPI link whose clock
 * and healing an app may choose.
 */
#pragma once

#include <stdbool.h>

#include "gfx/gfx.h"

/* QSPI clock for the panel: 16.5 ms of bus a full frame at 40 MHz, 8.2 at
 * 80. 80 needs every strip sent from internal DMA RAM (strip_bounce),
 * exceeds the panel's rated 50 MHz, and can leave stray pixels in a
 * partially redrawn frame; see
 * CONFIG_LAUNCHER_GFX_QSPI_80MHZ. The divider resolves to exactly 40 or 80,
 * hence a bool. The thresholds in gfx_dirty.h are fitted to 40 MHz. */
#if defined(CONFIG_LAUNCHER_GFX_QSPI_80MHZ) && CONFIG_LAUNCHER_GFX_QSPI_80MHZ
#define GFX_QSPI_HZ (80 * 1000 * 1000)
#else
#define GFX_QSPI_HZ (40 * 1000 * 1000)
#endif

/* Enable or disable partial clear mode. When enabled, gfx_clear() erases only
 * the bounding box of what was marked dirty on the previous frame instead of
 * wiping the entire 322 KiB framebuffer, and automatically marks that erased
 * region dirty for presentation. Off by default. */
void gfx_set_partial_clear(bool enabled);

/* Enable or disable interlace mode. When enabled, gfx_present() updates
 * only even-numbered strips on even frames and odd-numbered strips on odd
 * frames. Off by default. */
void gfx_set_interlace(bool enabled);
bool gfx_interlace_enabled(void);

/* Forces the next gfx_clear() to wipe the entire screen in full, resetting
 * partial clear tracking. Needed when an app opens, closes, or rotates. */
void gfx_invalidate(void);

/* One call for a transition instead of composing gfx_mark_all_dirty() and
 * gfx_invalidate() separately, plus a pending latch (below) an app's
 * optional invalidate() callback (app.h) answers to. Sets state only and
 * frees nothing, so it is safe from anywhere on core 0, an app callback or
 * a UI build included. */
void gfx_request_full_redraw(void);

/* True from a gfx_request_full_redraw() call until the shell clears it for
 * the pass that follows. */
bool gfx_full_redraw_pending(void);

/* Ends the window gfx_request_full_redraw() opened, called by the shell
 * once it has read the flag and decided whether to invoke an app's
 * invalidate(), before that pass's frame() runs. */
void gfx_full_redraw_clear_pending(void);

/*
 * gfx_present() sends only the horizontal bands that changed: the panel
 * holds the rest in its own GRAM, and sending is almost the whole cost of a
 * frame. Every gfx_* drawing call marks what it touched and gfx_clear()
 * marks the whole screen, so most callers never touch this. Code writing
 * through gfx_framebuffer() directly MUST mark what it wrote; forgetting
 * looks like a frozen or partially stale screen, not a crash.
 */

/* Declare that a rectangle of the framebuffer has changed. Tracked as a real
 * box per grid cell, not just which cell: a caller that knows it only
 * touched part of a cell may end up sending less than the whole thing. */
void gfx_mark_dirty(int x, int y, int w, int h);

void gfx_mark_all_dirty(void);

/* Whether any recorded dirt overlaps this rectangle. The tracker uses
 * leaf-sized regions for precise marks and full cells for band marks. */
bool gfx_region_dirty(int x, int y, int w, int h);

/* Send the changed bands to the panel and wait for the transfers to land.
 * The wait is mandatory; see the notes on asynchronous DMA in the docs.
 * Exactly gfx_present_begin() followed by gfx_present_wait(); every existing
 * caller keeps working unchanged under the core-1 present task below. */
void gfx_present(void);

/*
 * Split present: gfx_present_begin() hands the framebuffer to the core-1
 * present task and returns at once; gfx_present_wait() blocks until sent.
 * Between the two, no gfx_* call that touches drawing state or the
 * framebuffer may run: app.h's update() contract, asserted in development
 * builds (gfx_present_guard.h). gfx_set_present_async(false) sends
 * synchronously on the caller instead, for A/B measurement.
 */
void gfx_present_begin(void);
void gfx_present_wait(void);

void gfx_set_present_async(bool on);
bool gfx_present_async_enabled(void);

/* The panel link's clock. The fast rate halves bus time but is past the
 * panel's rated 50 MHz: a region can land with stray pixels that stay until
 * it is sent again. gfx starts at GFX_QSPI_HZ and keeps whatever it was
 * last told; it does not choose or remember a rate itself. */
#define GFX_PANEL_CLOCK_SLOW_HZ (40 * 1000 * 1000)
#define GFX_PANEL_CLOCK_FAST_HZ (80 * 1000 * 1000)

/* Takes effect before the next present sends anything, never mid-send.
 * Returns false, changing nothing, for any other rate. */
bool gfx_set_panel_clock_hz(int hz);
int gfx_panel_clock_hz(void);

/*
 * Heal, an opt-in for an app that sends only what changed: gfx sends a
 * marked region again on a later present, as full-width strips cut
 * differently from any earlier send, to clear what the fast clock left
 * wrong. All of it does nothing while the clock is the slow one.
 */

#define GFX_HEAL_DEFAULT_BUDGET_PIXELS (GFX_WIDTH * 32)

/* Queues rows [y, y + h) for healing; x and w are ignored, strips are full
 * width. Safe wherever gfx_mark_dirty() is. */
void gfx_heal_mark(int x, int y, int w, int h);

/* How many pixels of heal one present may add. */
void gfx_heal_set_budget(int pixels_per_present);

/* Rows per present of a sweep over the whole screen, 0 for none, for an app
 * that wants healing without a policy. */
void gfx_heal_set_rolling(int rows_per_present);

/* Empties the queue and puts the budget and rolling sweep back to their
 * defaults. */
void gfx_heal_restore_defaults(void);

bool gfx_heal_active(void);
