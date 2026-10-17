#include "shell/apps/diagnostics/ui/toggles_screen.h"

#include <stdio.h>

#include "build/build_variant.h"
#include "ui/ui.h"

#define TEXT_ROW_HEIGHT    (ui_text_height() + UI_TEXT_PAD)
#define HEADING_ROW_HEIGHT (ui_text_height() + (2 * UI_TEXT_PAD))
/* A paragraph's lines sit one text height apart, with no padding. */
#define NOTE_ROW_HEIGHT    ui_text_height()

/* Pre-broken to fit the portrait canvas, the narrower one: face text is one
 * clipped line per rectangle, and the pinned portrait render is what shows a
 * line that no longer fits. */
static const char* const PANEL_CLOCK_NOTE[] = {
    "80 MHz is faster;", "apps that redraw", "only what changed", "may show stray", "pixels or thin lines.",
};

static void
draw_text_row(int h, const char* s) {
    ui_row(h);
    ui_text(ui_next(), s);
}

static void
draw_toggle_row(const char* label, bool* value) {
    ui_row(UI_ROW_HEIGHT);
    ui_toggle(ui_next(), label, value);
}

/* Shows three things, not just the quarter: raw accelerometer counts,
 * derived gx/gy (what display_update() actually decides from), and the
 * shell's current quarter, so a hold reads as "this orientation gives
 * these numbers, shell calls it quarter N" in one line, without doing the
 * arithmetic by hand. */
static void
draw_orientation_readout(const toggles_screen_state_t* state) {
    char line[64];
    if (!state->imu_ready) {
        draw_text_row(TEXT_ROW_HEIGHT, "no IMU");
    } else if (!state->have_sample) {
        draw_text_row(TEXT_ROW_HEIGHT, "IMU read failed");
    } else {
        snprintf(line, sizeof line, "accel ax=%d ay=%d az=%d", state->accel_ax, state->accel_ay, state->accel_az);
        draw_text_row(TEXT_ROW_HEIGHT, line);
        snprintf(line, sizeof line, "gravity gx=%d gy=%d", state->gravity_gx, state->gravity_gy);
        draw_text_row(TEXT_ROW_HEIGHT, line);
    }
    snprintf(line, sizeof line, "shell quarter=%d", state->shell_quarter);
    draw_text_row(TEXT_ROW_HEIGHT, line);
}

#if CONFIG_LAUNCHER_SELFTEST
/* An ACTION, not a persistent toggle like the rows above; see
 * app_diagnostics.c's own comment on selftest_pending for why
 * selftest_run() itself never runs from inside this if-block. */
static void
draw_selftest_controls(const toggles_screen_state_t* state, toggles_screen_result_t* result) {
    ui_row(UI_ROW_HEIGHT);
    if (ui_button(ui_next(), "run self test suite")) {
        result->selftest_clicked = true;
    }

    char selftest_line[48];
    if (state->selftest_failures < 0) {
        snprintf(selftest_line, sizeof selftest_line, "self test: not run yet");
    } else if (state->selftest_failures == 0) {
        snprintf(selftest_line, sizeof selftest_line, "self test: all passed");
    } else {
        snprintf(selftest_line, sizeof selftest_line, "self test: %d failure(s)", state->selftest_failures);
    }
    draw_text_row(TEXT_ROW_HEIGHT, selftest_line);
}
#endif /* CONFIG_LAUNCHER_SELFTEST */

toggles_screen_result_t
toggles_screen_draw(const toggles_screen_state_t* state) {
    toggles_screen_result_t result = {
        .overlay_on = state->overlay_on,
        .leaf_on = state->leaf_on,
        .interlace_on = state->interlace_on,
        .fast_clock = state->fast_clock,
        .send_audit_on = state->send_audit_on,
        .show_orientation = state->show_orientation,
    };

    ui_screen();
    draw_text_row(HEADING_ROW_HEIGHT, "DEVELOPER TOGGLES");

    draw_toggle_row("gfx panel-grid overlay", &result.overlay_on);
    draw_toggle_row("gfx leaf-rect overlay", &result.leaf_on);
    draw_toggle_row("gfx interlace mode", &result.interlace_on);

    draw_toggle_row("panel clock 80 MHz (system)", &result.fast_clock);
    for (size_t i = 0; i < sizeof PANEL_CLOCK_NOTE / sizeof PANEL_CLOCK_NOTE[0]; i++) {
        draw_text_row(NOTE_ROW_HEIGHT, PANEL_CLOCK_NOTE[i]);
    }

    draw_toggle_row("gfx send audit (logs)", &result.send_audit_on);
    draw_toggle_row("show orientation", &result.show_orientation);

    if (result.show_orientation) {
        draw_orientation_readout(state);
    }

#if CONFIG_LAUNCHER_SELFTEST
    draw_selftest_controls(state, &result);
#endif

    draw_text_row(HEADING_ROW_HEIGHT, "BOOT for the POST report");
    return result;
}
