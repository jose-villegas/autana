/* The settings list's window and fixed text fit both canvas orientations. */
#include "suites.h"
#include "test_cleanup.h"
#include "ui_probe.h"
#include "unity.h"

#include "display/display.h"
#include "gfx/gfx.h"
#include "ui/layout/ui_transform.h"
#include "ui/ui.h"

#include "shell/apps/system_settings/ui/settings_layout.h"
#include "shell/apps/system_settings/ui/settings_screen.h"

static void
fixture(int quarter) {
    ui_init();
    ui_set_transform(ui_transform_quarter_turn(quarter, GFX_WIDTH, GFX_HEIGHT));
}

static void
test_window_and_rows_fit_both_canvases(void) {
    for (int quarter = DISPLAY_PORTRAIT; quarter <= DISPLAY_LANDSCAPE; quarter++) {
        fixture(quarter);
        const settings_layout_t layout = settings_layout(ui_width(), ui_height(), ui_text_height());
        TEST_ASSERT_GREATER_OR_EQUAL_INT(0, layout.window.x);
        TEST_ASSERT_GREATER_OR_EQUAL_INT(0, layout.window.y);
        TEST_ASSERT_LESS_OR_EQUAL_INT(ui_width(), layout.window.x + layout.window.w);
        TEST_ASSERT_LESS_OR_EQUAL_INT(ui_height(), layout.window.y + layout.window.h);
        TEST_ASSERT_GREATER_OR_EQUAL_INT(UI_TAP_MIN, layout.toggle_height);
        TEST_ASSERT_GREATER_OR_EQUAL_INT(ui_text_height(), layout.heading_height);
        TEST_ASSERT_GREATER_OR_EQUAL_INT(ui_text_height(), layout.note_height);
    }
}

/* The widget probe records rects on a host only. */
#ifndef ESP_PLATFORM
/* Draws the real list, recording where each toggle row landed. */
static void
draw_list(void) {
    const settings_snapshot_t snapshot = {0};
    const input_t input = {0};
    ui_begin(&input);
    (void)settings_screen_draw(&snapshot);
    ui_probe_end_frame();
}

/* A toggle draws its box as a square of the row's height and its label one
 * padding past it; text rows span the same window width as the toggles. */
static void
test_every_fixed_string_fits_its_row_at_both_canvases(void) {
    const char* const labels[] = {settings_panel_clock_label, settings_overlay_label, settings_leaf_label,
                                  settings_interlace_label, settings_send_audit_label};
    const char* const texts[] = {settings_display_heading,     settings_developer_heading,
                                 settings_panel_clock_note[0], settings_panel_clock_note[1],
                                 settings_panel_clock_note[2], settings_panel_clock_note[3],
                                 settings_panel_clock_note[4]};
    ui_probe_widgets_start();
    suite_add_test_cleanup(ui_probe_widgets_stop);
    for (int quarter = DISPLAY_PORTRAIT; quarter <= DISPLAY_LANDSCAPE; quarter++) {
        fixture(quarter);
        draw_list();
        recti_t row = {0};
        for (size_t i = 0; i < sizeof labels / sizeof labels[0]; i++) {
            TEST_ASSERT_TRUE_MESSAGE(ui_probe_widget(labels[i], &row), labels[i]);
            TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE(row.w - row.h - UI_PADDING, ui_measure_text(labels[i]), labels[i]);
        }
        for (size_t i = 0; i < sizeof texts / sizeof texts[0]; i++) {
            TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE(row.w, ui_measure_text(texts[i]), texts[i]);
        }
    }
}
#endif

void
run_settings_layout_suite(void) {
    RUN_TEST(test_window_and_rows_fit_both_canvases);
#ifndef ESP_PLATFORM
    RUN_TEST(test_every_fixed_string_fits_its_row_at_both_canvases);
#endif
}

SUITE_REGISTER(run_settings_layout_suite);
