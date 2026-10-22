#include "shell/apps/settings/ui/settings_layout.h"

#include "ui/ui.h"

const char settings_display_heading[] = "DISPLAY";
const char settings_developer_heading[] = "DEVELOPER";
const char settings_panel_clock_label[] = "panel clock 80 MHz (system)";
const char settings_overlay_label[] = "gfx panel-grid overlay";
const char settings_leaf_label[] = "gfx leaf-rect overlay";
const char settings_interlace_label[] = "gfx interlace mode";
const char settings_send_audit_label[] = "gfx send audit (logs)";
const char* const settings_panel_clock_note[SETTINGS_NOTE_LINE_COUNT] = {
    "80 MHz is faster;", "apps that redraw", "only what changed", "may show stray", "pixels or thin lines.",
};

settings_layout_t
settings_layout(int width, int height, int text_height) {
    const settings_layout_t layout = {
        .window = {0, 0, width, height},
        .row_width = width - (2 * (UI_MARGIN + UI_PADDING)),
        .heading_height = text_height + (2 * UI_TEXT_PAD),
        .toggle_height = UI_ROW_HEIGHT,
        .note_height = text_height,
    };
    return layout;
}
