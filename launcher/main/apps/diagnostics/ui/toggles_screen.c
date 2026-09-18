#include "toggles_screen.h"

#include <stdio.h>

#include "build_variant.h"
#include "gfx/gfx.h"
#include "ui/ui.h"

static void
draw_toggle_row(mu_Context* ctx, const char* label, bool* value) {
    mu_layout_row(ctx, 1, (int[]){-1}, UI_ROW_HEIGHT);
    int v = *value;
    mu_checkbox(ctx, label, &v);
    *value = v;
}

/* Shows three things, not just the quarter: raw accelerometer counts,
 * derived gx/gy (what display_update() actually decides from), and the
 * shell's current quarter - so a hold reads as "this orientation gives
 * these numbers, shell calls it quarter N" in one line, without doing the
 * arithmetic by hand. */
static void
draw_orientation_readout(mu_Context* ctx, const toggles_screen_state_t* state) {
    char line[64];
    mu_layout_row(ctx, 1, (int[]){-1}, gfx_text_height() + 4);

    if (!state->imu_ready) {
        mu_text(ctx, "no IMU");
    } else if (!state->have_sample) {
        mu_text(ctx, "IMU read failed");
    } else {
        snprintf(line, sizeof line, "accel ax=%d ay=%d az=%d", state->accel_ax, state->accel_ay, state->accel_az);
        mu_text(ctx, line);
        mu_layout_row(ctx, 1, (int[]){-1}, gfx_text_height() + 4);
        snprintf(line, sizeof line, "gravity gx=%d gy=%d", state->gravity_gx, state->gravity_gy);
        mu_text(ctx, line);
    }

    mu_layout_row(ctx, 1, (int[]){-1}, gfx_text_height() + 4);
    snprintf(line, sizeof line, "shell quarter=%d", state->shell_quarter);
    mu_text(ctx, line);
}

#if CONFIG_LAUNCHER_SELFTEST
/* An ACTION, not a persistent toggle like the checkboxes above - see
 * app_diagnostics.c's own comment on selftest_pending for why
 * selftest_run() itself never runs from inside this if-block. */
static void
draw_selftest_controls(mu_Context* ctx, const toggles_screen_state_t* state, toggles_screen_result_t* result) {
    mu_layout_row(ctx, 1, (int[]){-1}, UI_ROW_HEIGHT);
    if (mu_button(ctx, "run self test suite")) {
        result->selftest_clicked = true;
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
}
#endif /* CONFIG_LAUNCHER_SELFTEST */

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

        draw_toggle_row(ctx, "gfx panel-grid overlay", &result.overlay_on);
        draw_toggle_row(ctx, "gfx leaf-rect overlay", &result.leaf_on);
        draw_toggle_row(ctx, "gfx interlace mode", &result.interlace_on);

        draw_toggle_row(ctx, "panel clock 80 MHz (system)", &result.fast_clock);
        mu_layout_row(ctx, 1, (int[]){-1}, gfx_text_height() + 4);
        mu_text(ctx, "80 MHz is faster; apps that redraw only what changed may show stray pixels or thin lines.");

        draw_toggle_row(ctx, "gfx send audit (logs)", &result.send_audit_on);
        draw_toggle_row(ctx, "show orientation", &result.show_orientation);

        if (result.show_orientation) {
            draw_orientation_readout(ctx, state);
        }

#if CONFIG_LAUNCHER_SELFTEST
        draw_selftest_controls(ctx, state, &result);
#endif

        mu_layout_row(ctx, 1, (int[]){-1}, gfx_text_height() + 8);
        mu_text(ctx, "BOOT for the POST report");

        mu_end_window(ctx);
    }

    return result;
}
