#include "shell/apps/system_settings/ui/settings_layout.h"

#include "ui/ui.h"

const char settings_display_heading[] = "DISPLAY";
const char settings_developer_heading[] = "DEVELOPER";
const char settings_panel_clock_label[] = "fast panel clock";
const char settings_overlay_label[] = "grid overlay";
const char settings_leaf_label[] = "leaf overlay";
const char settings_interlace_label[] = "interlace";
const char settings_send_audit_label[] = "send audit";
const char* const settings_panel_clock_note[SETTINGS_NOTE_LINE_COUNT] = {
    "80 MHz is faster;", "apps that redraw", "only what changed", "may show stray", "pixels or lines.",
};

settings_layout_t
settings_layout(int width, int height, int text_height) {
    const settings_layout_t layout = {
        .window = {0, 0, width, height},
        .heading_height = text_height + (2 * UI_TEXT_PAD),
        .toggle_height = UI_ROW_HEIGHT,
        .note_height = text_height,
    };
    return layout;
}
