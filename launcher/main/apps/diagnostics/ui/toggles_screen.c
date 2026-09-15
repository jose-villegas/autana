#include "toggles_screen.h"

#include <stdio.h>

#include "gfx/gfx.h"
#include "ui/ui.h"

toggles_screen_result_t
toggles_screen_draw(mu_Context* ctx, const toggles_screen_state_t* state) {
    toggles_screen_result_t result = {
        .overlay_on = state->overlay_on,
        .leaf_on = state->leaf_on,
        .interlace_on = state->interlace_on,
        .fast_clock = state->fast_clock,
        .send_audit_on = state->send_audit_on,
        .show_orientation = state->show_orientation,
    };

    if (ui_begin_screen(ctx, "Developer Toggles", MU_OPT_NOTITLE | MU_OPT_NORESIZE | MU_OPT_NOCLOSE | MU_OPT_NOFRAME)) {

        mu_layout_row(ctx, 1, (int[]){-1}, gfx_text_height() + 8);
        mu_text(ctx, "DEVELOPER TOGGLES");

        mu_layout_row(ctx, 1, (int[]){-1}, UI_ROW_HEIGHT);
        int overlay_on = result.overlay_on;
        mu_checkbox(ctx, "gfx panel-grid overlay", &overlay_on);
        result.overlay_on = overlay_on;

        mu_layout_row(ctx, 1, (int[]){-1}, UI_ROW_HEIGHT);
        int leaf_on = result.leaf_on;
        mu_checkbox(ctx, "gfx leaf-rect overlay", &leaf_on);
        result.leaf_on = leaf_on;

        mu_layout_row(ctx, 1, (int[]){-1}, UI_ROW_HEIGHT);
        int interlace_on = result.interlace_on;
        mu_checkbox(ctx, "gfx interlace mode", &interlace_on);
        result.interlace_on = interlace_on;

        mu_layout_row(ctx, 1, (int[]){-1}, UI_ROW_HEIGHT);
        int fast_clock = result.fast_clock;
        mu_checkbox(ctx, "panel clock 80 MHz (system)", &fast_clock);
        result.fast_clock = fast_clock;
        mu_layout_row(ctx, 1, (int[]){-1}, gfx_text_height() + 4);
        mu_text(ctx, "80 MHz is faster; apps that redraw only what changed may show stray pixels or thin lines.");

        mu_layout_row(ctx, 1, (int[]){-1}, UI_ROW_HEIGHT);
        int send_audit_on = result.send_audit_on;
        mu_checkbox(ctx, "gfx send audit (logs)", &send_audit_on);
        result.send_audit_on = send_audit_on;

        mu_layout_row(ctx, 1, (int[]){-1}, UI_ROW_HEIGHT);
        int show_orientation = result.show_orientation;
        mu_checkbox(ctx, "show orientation", &show_orientation);
        result.show_orientation = show_orientation;

        /* Shows three things, not just the quarter: raw accelerometer
         * counts, derived gx/gy (what display_update() actually decides
         * from), and the shell's current quarter - so a hold reads as
         * "this orientation gives these numbers, shell calls it quarter
         * N" in one line, without doing the arithmetic by hand. */
        if (result.show_orientation) {
            char line[64];
            mu_layout_row(ctx, 1, (int[]){-1}, gfx_text_height() + 4);

            if (state->imu_ready) {
                if (state->have_sample) {
                    snprintf(line, sizeof line, "accel ax=%d ay=%d az=%d", state->accel_ax, state->accel_ay,
                             state->accel_az);
                    mu_text(ctx, line);
                    mu_layout_row(ctx, 1, (int[]){-1}, gfx_text_height() + 4);
                    snprintf(line, sizeof line, "gravity gx=%d gy=%d", state->gravity_gx, state->gravity_gy);
                    mu_text(ctx, line);
                } else {
                    mu_text(ctx, "IMU read failed");
                }
            } else {
                mu_text(ctx, "no IMU");
            }

            mu_layout_row(ctx, 1, (int[]){-1}, gfx_text_height() + 4);
            snprintf(line, sizeof line, "shell quarter=%d", state->shell_quarter);
            mu_text(ctx, line);
        }

#if CONFIG_LAUNCHER_SELFTEST
        /* An ACTION, not a persistent toggle like the checkboxes above -
         * see app_diagnostics.c's own comment on selftest_pending for why
         * selftest_run() itself never runs from inside this if-block. */
        mu_layout_row(ctx, 1, (int[]){-1}, UI_ROW_HEIGHT);
        if (mu_button(ctx, "run self test suite")) {
            result.selftest_clicked = true;
        }

        mu_layout_row(ctx, 1, (int[]){-1}, gfx_text_height() + 4);
        char selftest_line[48];
        if (state->selftest_failures < 0) {
            snprintf(selftest_line, sizeof selftest_line, "self test: not run yet");
        } else if (state->selftest_failures == 0) {
            snprintf(selftest_line, sizeof selftest_line, "self test: all passed");
        } else {
            snprintf(selftest_line, sizeof selftest_line, "self test: %d failure(s)", state->selftest_failures);
        }
        mu_text(ctx, selftest_line);
#endif /* CONFIG_LAUNCHER_SELFTEST */

        mu_layout_row(ctx, 1, (int[]){-1}, gfx_text_height() + 8);
        mu_text(ctx, "BOOT for the POST report");

        mu_end_window(ctx);
    }

    return result;
}
