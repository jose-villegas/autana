/* A whole settings screen: a title bar, then rows flowing down a window below it. */
#include "ui/ui.h"

typedef struct {
    int volume;
    bool muted;
    int output;
} settings_example_t;

static const char* const OUTPUTS[] = {"SPEAKER", "HEADPHONES", "BLUETOOTH", "OFF"};

bool
settings_example(settings_example_t* s) {
    ui_header((recti_t){0, 0, ui_width(), UI_TITLE_HEIGHT}, "SETTINGS");
    ui_window((recti_t){0, UI_TITLE_HEIGHT, ui_width(), ui_height() - UI_TITLE_HEIGHT});
    bool changed = false;
    ui_row(UI_ROW_HEIGHT);
    changed |= ui_slider(ui_next(), "volume", &s->volume, 0, 10);
    ui_row(UI_ROW_HEIGHT);
    changed |= ui_toggle(ui_next(), "MUTE", &s->muted);
    ui_row(UI_ROW_HEIGHT);
    changed |= ui_dropdown(ui_next(), "output", &s->output, OUTPUTS, 4);
    return changed;
}
