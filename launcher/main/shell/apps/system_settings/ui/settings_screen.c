#include "shell/apps/system_settings/ui/settings_screen.h"

#include "build/build_variant.h"
#include "shell/apps/system_settings/ui/settings_layout.h"
#include "ui/ui.h"

static void
draw_text_row(int width, int height, const char* text) {
    ui_row_centre(width, height);
    ui_text(ui_next(), text);
}

static void
draw_toggle_row(int width, int height, const char* label, bool* value) {
    ui_row_centre(width, height);
    ui_toggle(ui_next(), label, value);
}

settings_result_t
settings_screen_draw(const settings_snapshot_t* snapshot) {
    ui_scale_begin(SETTINGS_TEXT_SCALE);
    settings_result_t result = {.values = *snapshot};
    const settings_layout_t layout = settings_layout(ui_width(), ui_height(), ui_text_height());
    ui_scroll_window(layout.window);
    draw_text_row(layout.row_width, layout.heading_height, settings_display_heading);
    draw_toggle_row(layout.row_width, layout.toggle_height, settings_panel_clock_label, &result.values.fast_clock);
    for (int i = 0; i < SETTINGS_NOTE_LINE_COUNT; i++) {
        draw_text_row(layout.row_width, layout.note_height, settings_panel_clock_note[i]);
    }
#if BUILD_VARIANT_DEVELOPMENT_OR_HOST
    draw_text_row(layout.row_width, layout.heading_height, settings_developer_heading);
    draw_toggle_row(layout.row_width, layout.toggle_height, settings_overlay_label, &result.values.overlay_on);
    draw_toggle_row(layout.row_width, layout.toggle_height, settings_leaf_label, &result.values.leaf_on);
    draw_toggle_row(layout.row_width, layout.toggle_height, settings_interlace_label, &result.values.interlace_on);
    draw_toggle_row(layout.row_width, layout.toggle_height, settings_send_audit_label, &result.values.send_audit_on);
#endif
    ui_scale_end();
    return result;
}
