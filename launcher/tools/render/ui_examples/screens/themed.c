/* A screen in its own colours: ui_theme() once, then the same verbs as any other. */
#include "ui/ui.h"

typedef struct {
    int level;
    bool hard;
} themed_example_t;

static const ui_theme_t THEME = {
    .panel_face = 0x2A1A10,
    .panel_edge = 0xF2D6B0,
    .button_face = 0x3A2616,
    .accent = 0xFFB347,
    .accent_face = 0xFFB347,
    .on_accent = 0x2A1A06,
    .text = 0xFFF1E0,
    .caption = 0xC9A27A,
    .muted = 0x6B5340,
    .text_scale = 2,
    .icon_side = 24,
};

bool
themed_example(themed_example_t* s) {
    ui_theme(&THEME);
    ui_header((recti_t){0, 0, ui_width(), UI_TITLE_HEIGHT}, "NEW GAME");
    ui_window((recti_t){0, UI_TITLE_HEIGHT, ui_width(), ui_height() - UI_TITLE_HEIGHT});
    bool changed = false;
    ui_row(UI_ROW_HEIGHT);
    changed |= ui_slider(ui_next(), "level", &s->level, 0, 9);
    ui_row(UI_ROW_HEIGHT);
    changed |= ui_toggle(ui_next(), "HARD", &s->hard);
    ui_row(UI_ROW_HEIGHT);
    changed |= ui_button(ui_next(), "START");
    return changed;
}
