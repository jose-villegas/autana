#include "shell/apps/system_settings/ui/settings_screen.h"

#include "ui/ui.h"

static void
draw_toggle(const settings_toggle_t* row) {
    bool on = row->get();
    ui_row(UI_ROW_HEIGHT);
    if (ui_toggle(ui_next(), row->label, &on)) {
        row->set(on);
    }
}

void
settings_screen_draw(const settings_section_t* sections, int section_count) {
    ui_scroll_window((recti_t){0, 0, ui_width(), ui_height()});
    for (int s = 0; s < section_count; s++) {
        ui_row(UI_TITLE_HEIGHT);
        ui_header(ui_next(), sections[s].heading);
        for (int r = 0; r < sections[s].row_count; r++) {
            draw_toggle(&sections[s].rows[r]);
        }
    }
}
