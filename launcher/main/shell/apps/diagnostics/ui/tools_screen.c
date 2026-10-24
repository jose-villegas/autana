#include "shell/apps/diagnostics/ui/tools_screen.h"

#include <stdio.h>

#include "build/build_variant.h"
#include "ui/ui.h"

#define TEXT_ROW_HEIGHT    (ui_text_height() + UI_TEXT_PAD)
#define HEADING_ROW_HEIGHT (ui_text_height() + (2 * UI_TEXT_PAD))

static void
draw_text_row(int h, const char* s) {
    ui_row(h);
    ui_text(ui_next(), s);
}

/* Shows three things, not just the quarter: raw accelerometer counts,
 * derived gx/gy (what display_update() actually decides from), and the
 * shell's current quarter, so a hold reads as "this orientation gives
 * these numbers, shell calls it quarter N" on one screen, without doing the
 * arithmetic by hand. One value per line, so a full-scale reading fits the
 * portrait canvas's clipped text rows. */
static void
draw_orientation_readout(const diagnostics_tools_screen_state_t* state) {
    char line[32];
    if (!state->imu_ready) {
        draw_text_row(TEXT_ROW_HEIGHT, "no IMU");
    } else if (!state->have_sample) {
        draw_text_row(TEXT_ROW_HEIGHT, "IMU read failed");
    } else {
        const struct {
            const char* name;
            int value;
        } readings[] = {
            {"accel ax", state->accel_ax},     {"accel ay", state->accel_ay},     {"accel az", state->accel_az},
            {"gravity gx", state->gravity_gx}, {"gravity gy", state->gravity_gy},
        };

        for (size_t i = 0; i < sizeof readings / sizeof readings[0]; i++) {
            (void)snprintf(line, sizeof line, "%s=%d", readings[i].name, readings[i].value);
            draw_text_row(TEXT_ROW_HEIGHT, line);
        }
    }
    (void)snprintf(line, sizeof line, "shell quarter=%d", state->shell_quarter);
    draw_text_row(TEXT_ROW_HEIGHT, line);
}

#if CONFIG_LAUNCHER_SELFTEST
/* An ACTION, not a persistent toggle like the rows above; see
 * app_diagnostics.c's own comment on selftest_pending for why
 * selftest_run() itself never runs from inside this if-block. */
static void
draw_selftest_controls(const diagnostics_tools_screen_state_t* state, diagnostics_tools_screen_result_t* result) {
    ui_row(UI_ROW_HEIGHT);
    if (ui_button(ui_next(), "run self test suite")) {
        result->selftest_clicked = true;
    }

    char selftest_line[48];
    if (state->selftest_failures < 0) {
        (void)snprintf(selftest_line, sizeof selftest_line, "self test: not run yet");
    } else if (state->selftest_failures == 0) {
        (void)snprintf(selftest_line, sizeof selftest_line, "self test: all passed");
    } else {
        (void)snprintf(selftest_line, sizeof selftest_line, "self test: %d failure(s)", state->selftest_failures);
    }
    draw_text_row(TEXT_ROW_HEIGHT, selftest_line);
}
#endif /* CONFIG_LAUNCHER_SELFTEST */

diagnostics_tools_screen_result_t
diagnostics_tools_screen_draw(const diagnostics_tools_screen_state_t* state) {
    diagnostics_tools_screen_result_t result = {.show_orientation = state->show_orientation};

    ui_screen();
    ui_row(UI_ROW_HEIGHT);
    ui_toggle(ui_next(), "show orientation", &result.show_orientation);

    if (result.show_orientation) {
        draw_orientation_readout(state);
    }

#if CONFIG_LAUNCHER_SELFTEST
    draw_selftest_controls(state, &result);
#endif

    draw_text_row(HEADING_ROW_HEIGHT, "BOOT for the POST report");
    return result;
}
