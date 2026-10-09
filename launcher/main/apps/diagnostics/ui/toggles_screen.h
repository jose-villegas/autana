/*
 * toggles_screen: the developer-toggles page's microui drawing.
 *
 * app_diagnostics.c owns every gfx/shell/IMU getter, setter and persisted
 * value this page needs. This file turns a snapshot into microui rows and
 * reports the changed value.
 *
 * Self-test fields stay unconditional so every build shares one state and
 * result interface; toggles_screen.c alone decides whether to draw the row.
 */
#pragma once

#include <stdbool.h>

#include "core/build_variant.h"
#include "microui.h"

typedef struct {
    bool overlay_on;
    bool leaf_on;
    bool interlace_on;
    bool fast_clock;
    bool send_audit_on;
    bool show_orientation;

    /* have_sample requires imu_ready; accel and gravity require both. */
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

/* Caller brackets drawing with ui_begin()/ui_end(). */
toggles_screen_result_t toggles_screen_draw(mu_Context* ctx, const toggles_screen_state_t* state);
