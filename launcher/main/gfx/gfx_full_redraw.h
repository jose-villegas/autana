/*
 * gfx_full_redraw - the two latches behind gfx_request_full_redraw()
 * (gfx.h) that gfx_dirty.h does not already own: band mode's own
 * "force every band next frame" signal, and the shell-facing "a redraw
 * was requested and not yet consumed" flag.
 *
 * Header-only and static, the same reason gfx_dirty.h and
 * gfx_present_guard.h are: gfx.c includes this once and gets its own
 * pair of flags, a host suite includes it again and gets an independent
 * copy to drive and inspect directly, with no ESP-IDF dependency to
 * satisfy - see suite_gfx_full_redraw.c.
 */
#pragma once

#include <stdbool.h>

/* Independent of gfx_dirty.h's all_dirty, which full-fb's own present
 * already owns - band mode has no framebuffer for that tracker to
 * describe. Starts true so the very first band frame after boot, or
 * after gfx_mode_enter() grants band mode, forces every band regardless
 * of anything gfx_mark_dirty() has been told yet. */
static bool gfx_band_force_all_dirty = true;

static inline void
gfx_band_force_all(void) {
    gfx_band_force_all_dirty = true;
}

/* Reads and clears in one step: a request arriving after this frame's
 * capture must not retroactively force bands a frame already under way
 * already skipped - see gfx_band_frame_begin()'s own comment in gfx.c. */
static inline bool
gfx_band_take_force_all(void) {
    const bool forced = gfx_band_force_all_dirty;
    gfx_band_force_all_dirty = false;
    return forced;
}

/* True from a gfx_request_full_redraw() call until the shell consumes it
 * for the pass that follows - see gfx_full_redraw_pending()/
 * gfx_full_redraw_clear_pending() in gfx.h. An app's invalidate() callback
 * (app.h) fires at most once per request: the shell checks this before
 * the next frame() and clears it after, so a request made mid-frame is
 * picked up on the pass that follows rather than the one already running. */
static bool gfx_full_redraw_latched;

static inline void
gfx_full_redraw_latch(void) {
    gfx_full_redraw_latched = true;
}

static inline bool
gfx_full_redraw_is_pending(void) {
    return gfx_full_redraw_latched;
}

static inline void
gfx_full_redraw_unlatch(void) {
    gfx_full_redraw_latched = false;
}
