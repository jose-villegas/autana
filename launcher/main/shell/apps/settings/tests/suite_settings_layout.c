/* Fixed text fits its flowed row at both canvas orientations. */
#include "draw/draw_font_roles.h"
#include "gfx/gfx.h"
#include "shell/apps/settings/ui/settings_layout.h"
#include "suites.h"
#include "ui/ui.h"
#include "unity.h"

static void
assert_text_fits(const char* text, int width, int height) {
    TEST_ASSERT_LESS_OR_EQUAL_INT(width, draw_font_text_width(draw_font_ui(), text, -1, SETTINGS_TEXT_SCALE));
    TEST_ASSERT_LESS_OR_EQUAL_INT(height, draw_font_height(draw_font_ui(), SETTINGS_TEXT_SCALE));
}

static void
test_settings_geometry_and_strings_fit_both_canvases(void) {
    const int canvases[][2] = {{GFX_WIDTH, GFX_HEIGHT}, {GFX_HEIGHT, GFX_WIDTH}};
    for (size_t i = 0; i < sizeof canvases / sizeof canvases[0]; i++) {
        const int width = canvases[i][0], height = canvases[i][1];
        const settings_layout_t layout =
            settings_layout(width, height, draw_font_height(draw_font_ui(), SETTINGS_TEXT_SCALE));
        TEST_ASSERT_GREATER_OR_EQUAL_INT(0, layout.window.x);
        TEST_ASSERT_GREATER_OR_EQUAL_INT(0, layout.window.y);
        TEST_ASSERT_LESS_OR_EQUAL_INT(width, layout.window.x + layout.window.w);
        TEST_ASSERT_LESS_OR_EQUAL_INT(height, layout.window.y + layout.window.h);
        TEST_ASSERT_GREATER_OR_EQUAL_INT(UI_TAP_MIN, layout.toggle_height);
        TEST_ASSERT_GREATER_OR_EQUAL_INT(UI_TAP_MIN, layout.row_width);
        const char* const labels[] = {settings_panel_clock_label, settings_overlay_label, settings_leaf_label,
                                      settings_interlace_label, settings_send_audit_label};
        for (size_t j = 0; j < sizeof labels / sizeof labels[0]; j++) {
            assert_text_fits(labels[j], layout.row_width - layout.toggle_height - UI_PADDING, layout.toggle_height);
        }
        assert_text_fits(settings_display_heading, layout.row_width, layout.heading_height);
        assert_text_fits(settings_developer_heading, layout.row_width, layout.heading_height);
        for (int j = 0; j < SETTINGS_NOTE_LINE_COUNT; j++) {
            assert_text_fits(settings_panel_clock_note[j], layout.row_width, layout.note_height);
        }
    }
}

void
run_settings_layout_suite(void) {
    RUN_TEST(test_settings_geometry_and_strings_fit_both_canvases);
}

SUITE_REGISTER(run_settings_layout_suite);
