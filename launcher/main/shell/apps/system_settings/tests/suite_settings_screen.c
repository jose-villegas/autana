/* The real settings list driven through the face with stand-in owners: its
 * command-list budget, which owner a tap reaches, and that every fixed
 * string fits its row at both canvas orientations. */
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

#define ROW_COUNT 5

static bool values[ROW_COUNT];
static int set_calls[ROW_COUNT];

#define STUB_OWNER(i)                                                                                                  \
    static bool get_##i(void) { return values[i]; }                                                                    \
    static void set_##i(bool on) {                                                                                     \
        values[i] = on;                                                                                                \
        set_calls[i]++;                                                                                                \
    }
STUB_OWNER(0)
STUB_OWNER(1)
STUB_OWNER(2)
STUB_OWNER(3)
STUB_OWNER(4)

static const settings_toggle_t DISPLAY_ROWS[] = {
    {settings_panel_clock_label, get_0, set_0},
};
static const settings_toggle_t DEVELOPER_ROWS[] = {
    {settings_overlay_label, get_1, set_1},
    {settings_leaf_label, get_2, set_2},
    {settings_interlace_label, get_3, set_3},
    {settings_send_audit_label, get_4, set_4},
};
static const settings_section_t SECTIONS[] = {
    {settings_display_heading, DISPLAY_ROWS, 1},
    {settings_developer_heading, DEVELOPER_ROWS, ROW_COUNT - 1},
};
#define SECTION_COUNT ((int)(sizeof SECTIONS / sizeof SECTIONS[0]))

static void
fixture(int quarter, bool on) {
    ui_init();
    ui_set_transform(ui_transform_quarter_turn(quarter, GFX_WIDTH, GFX_HEIGHT));
    for (int i = 0; i < ROW_COUNT; i++) {
        values[i] = on;
        set_calls[i] = 0;
    }
}

static void
test_settings_list_command_list_fits_budget(void) {
    for (int quarter = DISPLAY_PORTRAIT; quarter <= DISPLAY_LANDSCAPE; quarter++) {
        fixture(quarter, true);
        const input_t input = {0};
        ui_begin(&input);
        settings_screen_draw(SECTIONS, SECTION_COUNT);
        ui_probe_budget_assert("settings list");
    }
}

#if UI_PROBE_WIDGETS_RECORDED
/* Landscape (448x368 logical): this project's shipping orientation. */
#define FIXTURE_QUARTER 1
/* Finer than any row, so every row on the canvas is tapped at least once. */
#define TAP_STEP_PX     (UI_ROW_HEIGHT / 8)

static const char* const LABELS[ROW_COUNT] = {settings_panel_clock_label, settings_overlay_label, settings_leaf_label,
                                              settings_interlace_label, settings_send_audit_label};

static void
frame(const input_t* in) {
    ui_begin(in);
    settings_screen_draw(SECTIONS, SECTION_COUNT);
    ui_probe_end_frame();
}

/* Which row's setter one press-and-release at (x, y) called, -1 for none;
 * fails if it called more than one, or set a value other than the flip. */
static int
tap(int x, int y) {
    const input_t idle = {0};
    const input_t press = {.down = true, .pressed = true, .x = x, .y = y, .press_x = x, .press_y = y};
    const input_t release = {.released = true, .x = x, .y = y, .press_x = x, .press_y = y};
    bool before[ROW_COUNT];
    for (int i = 0; i < ROW_COUNT; i++) {
        before[i] = values[i];
        set_calls[i] = 0;
    }
    frame(&idle);
    frame(&press);
    frame(&release);
    int called = -1;
    for (int i = 0; i < ROW_COUNT; i++) {
        if (set_calls[i] != 0) {
            TEST_ASSERT_EQUAL_INT_MESSAGE(-1, called, "one tap called more than one setter");
            TEST_ASSERT_EQUAL_INT_MESSAGE(1, set_calls[i], "one tap called a setter twice");
            TEST_ASSERT_NOT_EQUAL_MESSAGE(before[i], values[i], "a tap set the value it already had");
            called = i;
        }
    }
    return called;
}

/* Every row reaches only its own owner, rows bind top to bottom, and each
 * row drawn wholly on the canvas is reachable, from either starting value. */
static void
test_a_tap_on_a_row_calls_only_that_rows_owner(void) {
    ui_probe_widgets_start();
    suite_add_test_cleanup(ui_probe_widgets_stop);
    for (int on = 0; on <= 1; on++) {
        fixture(FIXTURE_QUARTER, on);
        frame(&(input_t){0});
        bool visible[ROW_COUNT];
        for (int i = 0; i < ROW_COUNT; i++) {
            recti_t r;
            visible[i] = ui_probe_widget(LABELS[i], &r) && r.y >= 0 && r.y + r.h <= ui_height();
        }

        /* Taps arrive in panel space, as the touch controller reports them. */
        const affine2x_t t = ui_transform_quarter_turn(FIXTURE_QUARTER, GFX_WIDTH, GFX_HEIGHT);
        bool reached[ROW_COUNT] = {false};
        int last = -1;
        for (int y = 0; y < ui_height(); y += TAP_STEP_PX) {
            const vec2i_t panel = affine2x_map_pixel(&t, (vec2i_t){ui_width() / 2, y});
            const int row = tap(panel.x, panel.y);
            if (row >= 0) {
                TEST_ASSERT_TRUE_MESSAGE(row >= last, "a lower tap reached a higher row");
                last = row;
                reached[row] = true;
            }
        }
        for (int i = 0; i < ROW_COUNT; i++) {
            if (visible[i]) {
                TEST_ASSERT_TRUE_MESSAGE(reached[i], LABELS[i]);
            }
        }
        TEST_ASSERT_TRUE_MESSAGE(visible[0], "the first row is below the fold");
    }
}

/* Rows below the fold are not drawn, so each label is measured at the
 * offset the first label is drawn at inside its row; every row
 * of the window is the same width. */
static void
test_every_fixed_string_fits_its_row_at_both_canvases(void) {
    ui_probe_widgets_start();
    suite_add_test_cleanup(ui_probe_widgets_stop);
    for (int quarter = DISPLAY_PORTRAIT; quarter <= DISPLAY_LANDSCAPE; quarter++) {
        fixture(quarter, false);
        frame(&(input_t){0});
        recti_t row;
        recti_t label;
        TEST_ASSERT_TRUE(ui_probe_widget(settings_panel_clock_label, &row));
        TEST_ASSERT_TRUE(ui_probe_text(settings_panel_clock_label, &label));
        const int label_room = row.x + row.w - label.x;
        for (int i = 0; i < ROW_COUNT; i++) {
            TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE(label_room, ui_measure_text(LABELS[i]), LABELS[i]);
        }
        for (int s = 0; s < SECTION_COUNT; s++) {
            TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE(row.w, ui_measure_text(SECTIONS[s].heading), SECTIONS[s].heading);
        }
    }
}
#endif

void
run_settings_screen_suite(void) {
    RUN_TEST(test_settings_list_command_list_fits_budget);
#if UI_PROBE_WIDGETS_RECORDED
    RUN_TEST(test_a_tap_on_a_row_calls_only_that_rows_owner);
    RUN_TEST(test_every_fixed_string_fits_its_row_at_both_canvases);
#endif
}

SUITE_REGISTER(run_settings_screen_suite);
