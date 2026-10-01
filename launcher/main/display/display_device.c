/*
 * display_device: display.h's shell side, over the panel driver and the
 * settings store. Device only: the pure orientation decision stays in
 * display.c.
 */

#include "display/display.h"

#include "display/panel_clock.h"
#include "gfx/gfx.h"
#include "input/input.h"
#include "util/log.h"
#include "util/memory.h"
#include "util/settings.h"

static const char* TAG = "display";

/* 10 Hz: sufficient for reorientation without lag. */
#define DISPLAY_SAMPLE_MS          100

#define PANEL_CLOCK_SETTINGS_SPACE "shell"
#define PANEL_CLOCK_SETTINGS_KEY   "panel_hz"

_Static_assert(PANEL_CLOCK_SLOW_HZ == GFX_PANEL_CLOCK_SLOW_HZ && PANEL_CLOCK_FAST_HZ == GFX_PANEL_CLOCK_FAST_HZ,
               "panel_clock.h's rates must match gfx.h's");

static display_t shell_display;
static int64_t next_sample_us;
static panel_clock_t shell_panel_clock;

static void
load_system_panel_clock(void) {
    int32_t saved = 0;
    const bool found = settings_read_i32(PANEL_CLOCK_SETTINGS_SPACE, PANEL_CLOCK_SETTINGS_KEY, &saved);
    panel_clock_init(&shell_panel_clock, found, saved, GFX_QSPI_HZ);
    gfx_set_panel_clock_hz(panel_clock_system_hz(&shell_panel_clock));
}

bool
display_start(void) {
    if (!gfx_init()) {
        memory_dump(MEMORY_DMA);
        return false;
    }
    load_system_panel_clock();
    return true;
}

void
display_reset_quarter(void) {
    display_init(&shell_display);
    shell_display.quarter = DISPLAY_DEFAULT_QUARTER;
}

bool
display_sample_orientation(int64_t now_us) {
    if (now_us < next_sample_us) {
        return false;
    }
    next_sample_us = now_us + (int64_t)DISPLAY_SAMPLE_MS * 1000;

    imu_sample_t sample;
    if (!input_read_motion(&sample)) {
        return false;
    }
    return display_update(&shell_display, imu_gravity_screen_x(&sample), imu_gravity_screen_y(&sample));
}

int
display_quarter_now(void) {
    return display_quarter(&shell_display);
}

int
display_system_panel_clock_hz(void) {
    return panel_clock_system_hz(&shell_panel_clock);
}

void
display_set_system_panel_clock_hz(int hz) {
    if (hz == panel_clock_system_hz(&shell_panel_clock) || !panel_clock_set_system(&shell_panel_clock, hz)) {
        return;
    }
    gfx_set_panel_clock_hz(hz);
    if (!settings_write_i32(PANEL_CLOCK_SETTINGS_SPACE, PANEL_CLOCK_SETTINGS_KEY, hz)) {
        log_warn(TAG, "could not save the panel clock choice");
    }
}

void
display_restore_system_state(void) {
    gfx_set_panel_clock_hz(panel_clock_for_switch(&shell_panel_clock));
    gfx_heal_restore_defaults();
}
