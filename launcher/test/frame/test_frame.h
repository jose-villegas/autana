/*
 * test_frame: the shell's side of a frame, as an app's test drives it: the
 * present, what it sent, the panel's clock and a pinned orientation. App
 * tests include this, never gfx/present/ or display/display_shell.h; the
 * present and framebuffer guards come through gfx_shared_state.h.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app/app.h"
#include "display/display_shell.h"
#include "gfx/gfx.h"
#include "gfx/present/gfx_debug.h"
#include "gfx/present/gfx_mode.h"
#include "gfx/present/gfx_present.h"
#include "gfx/present/modes/indexed/gfx_indexed_expand.h"
#include "shell/shell_apps.h"
#include "ui/layout/ui_transform.h"
#include "ui/ui.h"

/* Inline, so a timed present costs what the shell's own does. */
static inline void
test_present(void) {
    gfx_present();
}

static inline void
test_present_begin(void) {
    gfx_present_begin();
}

static inline void
test_present_wait(void) {
    gfx_present_wait();
}

/* Puts the last frame drawn on the panel for a readback: a double
 * framebuffer sends its back buffer now, with clear-on-send off so the frame
 * stays; the other layouts already show it. */
void test_present_keep_frame(void);

/* Answers a pending gfx_request_full_redraw() the way the shell does before
 * `app` draws its next frame. */
static inline void
test_apply_full_redraw(const app_t* app) {
    shell_apply_full_redraw(app);
}

/* What the presents since test_send_stats_reset() sent: strips sent whole,
 * as gathered runs or at less than a band's height, and panel bytes; plus
 * the last present's send and core-0 wait. */
typedef struct {
    int full_bands;
    int gathered;
    int partial_bands;
    int64_t bytes;
    int32_t send_us;
    int32_t wait_us;
} test_send_stats_t;

static inline void
test_send_stats_reset(void) {
    gfx_reset_strip_send_counts();
}

static inline test_send_stats_t
test_send_stats(void) {
    test_send_stats_t s = {
        .bytes = gfx_get_bytes_sent(), .send_us = gfx_get_present_send_us(), .wait_us = gfx_get_present_wait_us()};
    gfx_get_strip_send_counts(&s.full_bands, &s.gathered, &s.partial_bands);
    return s;
}

/* A timed present measures a known panel clock, not the system's choice of
 * one; the selftest runner's tearDown() puts the system clock back. Presents
 * once at hz, so the link reopen lands outside any timed window. */
static inline void
test_panel_clock_pin(int hz) {
    gfx_set_panel_clock_hz(hz);
    gfx_present();
}

/* Pins the panel to `quarter`, sensor ignored, and turns the UI to match. */
static inline void
test_hold_quarter(int quarter) {
    display_hold_quarter(quarter);
    ui_set_transform(ui_transform_quarter_turn(quarter, GFX_WIDTH, GFX_HEIGHT));
}

static inline void
test_release_quarter(void) {
    display_release_quarter();
}

static inline bool
test_quarter_held(void) {
    return display_quarter_held();
}
