/* Orientation readout and optional self-test controls. */
#pragma once

#include <stdbool.h>

#include "build/build_variant.h"

typedef struct {
    bool show_orientation;

    /* have_sample requires imu_ready; accel and gravity require both. */
    bool imu_ready;
    bool have_sample;
    int accel_ax, accel_ay, accel_az;
    int gravity_gx, gravity_gy;
    int shell_quarter;

    int selftest_failures; /* -1: never run yet. Read only in a SELFTEST build. */
} diagnostics_tools_screen_state_t;

typedef struct {
    bool show_orientation;
    bool selftest_clicked; /* meaningful only in a SELFTEST build */
} diagnostics_tools_screen_result_t;

/* The colour the caller passes to ui_end() for this page. */
#define DIAGNOSTICS_TOOLS_SCREEN_BACKGROUND 0x0A0C14

/* Caller brackets drawing with ui_begin()/ui_end(). */
diagnostics_tools_screen_result_t diagnostics_tools_screen_draw(const diagnostics_tools_screen_state_t* state);
