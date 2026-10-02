/*
 * display_device: display.h's shell side, over the panel driver and the
 * settings store. Device only: the pure orientation decision stays in
 * display.c.
 */

#include "display/display.h"

#include "build_variant.h"
#include "display/panel_clock.h"
#include "gfx/gfx.h"
#include "input/input_shell.h"
#include "util/log.h"
#include "util/memory.h"
#include "util/settings.h"

static const char TAG[] = "display";

#define PANEL_CLOCK_SETTINGS_SPACE "shell"
#define PANEL_CLOCK_SETTINGS_KEY   "panel_hz"

_Static_assert(PANEL_CLOCK_SLOW_HZ == GFX_PANEL_CLOCK_SLOW_HZ && PANEL_CLOCK_FAST_HZ == GFX_PANEL_CLOCK_FAST_HZ,
               "panel_clock.h's rates must match gfx.h's");

static display_orientation_t shell_orientation;
static panel_clock_t shell_panel_clock;

bool
display_start(void) {
    if (!gfx_init()) {
#if CONFIG_LAUNCHER_DEVELOPMENT
        memory_dump(MEMORY_DMA);
#endif
        return false;
    }
    return true;
}

void
display_load_panel_clock(void) {
    int32_t saved = 0;
    const bool found = settings_read_i32(PANEL_CLOCK_SETTINGS_SPACE, PANEL_CLOCK_SETTINGS_KEY, &saved);
    panel_clock_init(&shell_panel_clock, found, saved, GFX_QSPI_HZ);
    gfx_set_panel_clock_hz(panel_clock_system_hz(&shell_panel_clock));
}

void
display_reset_quarter(void) {
    display_orientation_init(&shell_orientation);
}

bool
display_sample_orientation(int64_t now_us) {
    return display_orientation_sample(&shell_orientation, now_us, input_read_motion);
}

int
display_quarter_now(void) {
    return display_quarter(&shell_orientation.display);
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
