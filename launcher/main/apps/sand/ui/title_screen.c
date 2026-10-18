#include "title_screen.h"

#include "draw/draw_font_roles.h"
#include "ui/ui.h"

#include "apps/sand/ui/sand_icons.h"
#include "math/scalar/mathi.h"
#include "sand_theme.h"

#define HEADER_H        48
#define SUBTITLE_GAP    12
#define SUBTITLE_H      24
#define MAIN_W_MAX      320
#define MAIN_H          UI_TAP_MIN
#define MAIN_GAP        10
#define MAIN_COUNT      3
#define FOOTER_W_MAX    360
#define FOOTER_H        (UI_TAP_MIN + 2 * FOOTER_PAD)
#define FOOTER_PAD      8
#define TITLE_SCALE_MAX 3

static const char* const LABELS[SAND_TITLE_BUTTON_COUNT] = {
    [SAND_TITLE_START] = "[ START GAME ]", [SAND_TITLE_LOAD] = "[ LOAD SAVES ]", [SAND_TITLE_OPTIONS] = "[ OPTIONS ]",
    [SAND_TITLE_GUIDE] = "GUIDE",          [SAND_TITLE_EXIT] = "EXIT",
};

static const sand_icon_id_t ICONS[SAND_TITLE_BUTTON_COUNT] = {
    [SAND_TITLE_START] = SAND_ICON_START, [SAND_TITLE_LOAD] = SAND_ICON_LOAD, [SAND_TITLE_OPTIONS] = SAND_ICON_OPTIONS,
    [SAND_TITLE_GUIDE] = SAND_ICON_GUIDE, [SAND_TITLE_EXIT] = SAND_ICON_EXIT,
};

const char*
title_screen_label(sand_title_button_t button) {
    return LABELS[button];
}

bool
title_screen_button_enabled(sand_title_button_t button) {
    return button != SAND_TITLE_LOAD && button != SAND_TITLE_GUIDE;
}

static int
largest_title_scale(int room) {
    int scale = TITLE_SCALE_MAX;
    while (scale > 1 && draw_font_text_width(draw_font_ui(), TITLE_SCREEN_TITLE, -1, scale) > room) {
        scale--;
    }
    return scale;
}

static void
layout_footer(int screen_w, int screen_h, title_screen_layout_t* out) {
    const int w = mathi_min(screen_w - 2 * UI_MARGIN, FOOTER_W_MAX);
    out->footer = ui_centered_rect(screen_w, w, FOOTER_H, screen_h - UI_MARGIN - FOOTER_H);

    const int button_w = (w - 3 * FOOTER_PAD) / 2;
    const int y = out->footer.y + FOOTER_PAD;
    const int h = FOOTER_H - 2 * FOOTER_PAD;
    out->buttons[SAND_TITLE_GUIDE] = (recti_t){out->footer.x + FOOTER_PAD, y, button_w, h};
    out->buttons[SAND_TITLE_EXIT] = (recti_t){out->footer.x + out->footer.w - FOOTER_PAD - button_w, y, button_w, h};
}

void
title_screen_layout(int screen_w, int screen_h, title_screen_layout_t* out) {
    out->header = (recti_t){0, 0, screen_w, HEADER_H};
    out->title_scale = largest_title_scale(screen_w - 2 * UI_MARGIN);
    out->subtitle = (recti_t){UI_MARGIN, HEADER_H + SUBTITLE_GAP, screen_w - 2 * UI_MARGIN, SUBTITLE_H};
    layout_footer(screen_w, screen_h, out);

    const int stack_h = MAIN_COUNT * MAIN_H + (MAIN_COUNT - 1) * MAIN_GAP;
    const int room_top = out->subtitle.y + out->subtitle.h;
    const int top = room_top + (out->footer.y - room_top - stack_h) / 2;
    const int w = mathi_min(screen_w - 2 * UI_MARGIN, MAIN_W_MAX);
    for (int i = 0; i < MAIN_COUNT; i++) {
        out->buttons[SAND_TITLE_START + i] = ui_centered_rect(screen_w, w, MAIN_H, top + i * (MAIN_H + MAIN_GAP));
    }
}

sand_title_button_t
title_screen_draw(void) {
    ui_set_text_style(UI_TEXT_PLAIN);

    title_screen_layout_t lay;
    title_screen_layout(ui_width(), ui_height(), &lay);

    ui_screen();
    ui_theme(&sand_ui_theme);
    ui_scale_begin(lay.title_scale);
    ui_header(lay.header, TITLE_SCREEN_TITLE);
    ui_scale_end();
    ui_color_begin(UI_COLOR_TEXT, SAND_THEME_SELECTED_COLOR);
    ui_text_centre(lay.subtitle, TITLE_SCREEN_SUBTITLE);
    ui_color_end();
    ui_panel(lay.footer);

    sand_title_button_t hit = SAND_TITLE_NONE;
    for (int i = 0; i < SAND_TITLE_BUTTON_COUNT; i++) {
        ui_disabled_begin(!title_screen_button_enabled((sand_title_button_t)i));
        if (ui_button_icon(lay.buttons[i], LABELS[i], sand_icon(ICONS[i]))) {
            hit = (sand_title_button_t)i;
        }
        ui_disabled_end();
    }
    return hit;
}
