/*
 * ui_scroll - a screen with more rows than fit. Only a RELATIVE mu_Rect
 * (mu_layout_set_next(), microui.c) folds into a container's content_size
 * and follows its scroll afterward, so only a RELATIVE rect can ever be
 * reached once the stack overflows the screen.
 *
 * ui_flow_row() is that fix, generalised to any centred fixed-width row.
 * ui_scroll_view_begin()/_end() sit around ui_begin_screen() to add what a
 * screen actually wants control over: whether the scrollbar is drawn, which
 * axis may move, and how a drag coasts after release - none of which
 * microui itself exposes a knob for, and none of which needs microui
 * patched, since every knob here is either a style value already read once
 * per window (scrollbar_size) or bookkeeping layered on top of a container's
 * own already-public scroll/content_size fields.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "microui.h"

typedef enum {
    UI_SCROLL_AXIS_NONE = 0,
    UI_SCROLL_AXIS_VERTICAL,
    UI_SCROLL_AXIS_HORIZONTAL,
    UI_SCROLL_AXIS_BOTH,
} ui_scroll_axis_t;

typedef struct {
    ui_scroll_axis_t axis; /* which axis(es) a drag or scrollbar may move */
    bool hide_scrollbar;   /* draw no scrollbar chrome; dragging the content still scrolls it */

    /* 0 (default): a drag stops dead on release. Above 0: residual
     * velocity decays exponentially with this time constant (ms to fall to
     * 1/e) instead - integrated in closed form from `dt_ms`, not stepped
     * per frame, so the coast covers the same distance at any frame rate. */
    uint32_t momentum_tau_ms;
} ui_scroll_view_config_t;

/* Vertical only, scrollbar visible, no momentum - what every screen already
 * got from microui unmodified, and what a screen not opting into anything
 * else here keeps getting. */
ui_scroll_view_config_t ui_scroll_view_default(void);

/* ui_begin_screen(), with `config` layered on top and `dt_ms` driving the
 * momentum coast. Returns what ui_begin_screen() returns. Close with
 * ui_scroll_view_end(), not mu_end_window() directly - it also applies this
 * frame's axis lock and momentum step, which needs the container
 * ui_begin_screen() just opened. */
int ui_scroll_view_begin(mu_Context* ctx, const char* title, int opt, ui_scroll_view_config_t config, uint32_t dt_ms);

/* Closes the window ui_scroll_view_begin() opened. */
void ui_scroll_view_end(mu_Context* ctx);

/* Forgets any in-flight momentum coast. A device session runs one
 * mu_Context for its whole lifetime, where a container's identity never
 * recurs, so nothing in the shell itself needs this - it exists for a test
 * fixture that re-mu_init()s the same mu_Context between cases and would
 * otherwise see the previous case's coast still bleeding into the next. */
void ui_scroll_reset_momentum(void);

/* A cursor for a centred column of rows that FLOW: each ui_flow_row() call
 * lands the next rect `flow->gap` below the last, at `flow->canvas_w`'s
 * centre. Pure bookkeeping, reusable across screens with no ctx or window
 * needed to compute WHERE a row lands, only to place it. */
typedef struct {
    int canvas_w;
    int y;
    int gap;
} ui_flow_t;

/* A cursor starting at `y0`, `gap` between rows. */
ui_flow_t ui_flow_start(int canvas_w, int y0, int gap);

/* The rect a stack of `count` rows of `row_h` each, `gap` apart, should
 * start at so the whole stack sits centred in `canvas_h` - or `margin` from
 * the top once it no longer fits. One centre-or-pin rule for every screen. */
int ui_flow_top(int canvas_h, int count, int row_h, int gap, int margin);

/* The next `w`x`h` row, centred on `flow->canvas_w`, and folded into the
 * enclosing window's content_size and scroll via mu_layout_set_next()'s
 * RELATIVE mode - see this header's own top comment. Returns the rect AT
 * REST (scroll 0), which is also what a caller needs for a fixed hit-test
 * rect that only makes sense before any scrolling has happened (a
 * self-test tapping row 0 right after entering a menu, say). Advances
 * `flow` for the next call. */
mu_Rect ui_flow_row(mu_Context* ctx, ui_flow_t* flow, int w, int h);
