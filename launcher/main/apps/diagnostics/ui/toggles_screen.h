/*
 * toggles_screen - the developer-toggles page's microui drawing.
 *
 * app_diagnostics.c owns every gfx/shell/IMU getter and setter this page
 * touches (gfx_debug_overlay(), shell_system_panel_clock_hz(), imu_read(),
 * ...) and the one persisted toggle (show_orientation); this file only
 * turns a snapshot of them into mu_checkbox()/mu_text() rows and reports
 * which value changed, the split docs/Building-a-Screen.md asks every
 * screen to keep.
 *
 * The self-test button and its result line are narrower still - drawn only
 * in a CONFIG_LAUNCHER_SELFTEST build, guarded in toggles_screen.c itself
 * rather than here: this header is included before any file in a
 * translation unit has necessarily pulled in the config macro yet (a
 * `#if CONFIG_LAUNCHER_SELFTEST` at parse time here saw it as always
 * undefined once, silently dropping the fields for every includer), so its
 * two fields below stay unconditional - a few unused bytes outside a
 * SELFTEST build, never a missing member. A host build (CONFIG_LAUNCHER_
 * SELFTEST never defined there) measures this page without the row they
 * feed; only a SELFTEST build's own run of tests/suite_command_list_budget.c
 * exercises it.
 */
#pragma once

#include <stdbool.h>

#include "build_variant.h"
#include "microui.h"

typedef struct {
    bool overlay_on;
    bool leaf_on;
    bool interlace_on;
    bool fast_clock;
    bool send_audit_on;
    bool show_orientation;

    /* Read once by app_diagnostics.c only while show_orientation is on -
     * see its own comment for why imu_read() is not called every frame.
     * have_sample is meaningless unless imu_ready is true, and the accel/
     * gravity fields are meaningless unless have_sample is also true. */
    bool imu_ready;
    bool have_sample;
    int accel_ax, accel_ay, accel_az;
    int gravity_gx, gravity_gy;
    int shell_quarter;

    int selftest_failures; /* -1: never run yet. Read only in a SELFTEST build. */
} toggles_screen_state_t;

typedef struct {
    bool overlay_on;
    bool leaf_on;
    bool interlace_on;
    bool fast_clock;
    bool send_audit_on;
    bool show_orientation;
    bool selftest_clicked; /* meaningful only in a SELFTEST build */
} toggles_screen_result_t;

/* Draws every row and reports each toggle's value after this frame's tap,
 * if any - app_diagnostics.c applies whichever changed to its own state
 * and the real gfx/shell setters. Caller brackets this with
 * ui_begin()/ui_end(). */
toggles_screen_result_t toggles_screen_draw(mu_Context* ctx, const toggles_screen_state_t* state);
