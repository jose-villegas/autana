#include "title_screen.h"

#include "gfx/gfx_font_roles.h"
#include "ui/ui.h"
#include "ui/ui_widgets.h"

#include "apps/sand/icons_sand.h"
#include "sand_theme.h"

#define HEADER_H        48
#define SUBTITLE_GAP    12
#define SUBTITLE_H      24
#define MAIN_W_MAX      320
#define MAIN_H          52
#define MAIN_GAP        10
#define MAIN_COUNT      3
#define FOOTER_W_MAX    360
#define FOOTER_H        64
#define FOOTER_PAD      8
#define TITLE_SCALE_MAX 3

static const char* const LABELS[SAND_TITLE_BUTTON_COUNT] = {
    [SAND_TITLE_START] = "[ START GAME ]", [SAND_TITLE_LOAD] = "[ LOAD SAVES ]", [SAND_TITLE_OPTIONS] = "[ OPTIONS ]",
    [SAND_TITLE_GUIDE] = "GUIDE",          [SAND_TITLE_EXIT] = "EXIT",
};

static const icon_sand_id_t ICONS[SAND_TITLE_BUTTON_COUNT] = {
    [SAND_TITLE_START] = ICON_SAND_START, [SAND_TITLE_LOAD] = ICON_SAND_LOAD, [SAND_TITLE_OPTIONS] = ICON_SAND_OPTIONS,
    [SAND_TITLE_GUIDE] = ICON_SAND_GUIDE, [SAND_TITLE_EXIT] = ICON_SAND_EXIT,
};

static const char* const IDS[SAND_TITLE_BUTTON_COUNT] = {
    [SAND_TITLE_START] = "title start", [SAND_TITLE_LOAD] = "title load", [SAND_TITLE_OPTIONS] = "title options",
    [SAND_TITLE_GUIDE] = "title guide", [SAND_TITLE_EXIT] = "title exit",
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
min_int(int a, int b) {
    return a < b ? a : b;
}

static int
largest_title_scale(int room) {
    int scale = TITLE_SCALE_MAX;
    while (scale > 1 && gfx_font_text_width(gfx_font_ui(), TITLE_SCREEN_TITLE, -1, scale) > room) {
        scale--;
    }
    return scale;
}

static void
layout_footer(int screen_w, int screen_h, title_screen_layout_t* out) {
    const int w = min_int(screen_w - 2 * UI_MARGIN, FOOTER_W_MAX);
    out->footer = ui_centered_rect(screen_w, w, FOOTER_H, screen_h - UI_MARGIN - FOOTER_H);

    const int button_w = (w - 3 * FOOTER_PAD) / 2;
    const int y = out->footer.y + FOOTER_PAD;
    const int h = FOOTER_H - 2 * FOOTER_PAD;
    out->buttons[SAND_TITLE_GUIDE] = mu_rect(out->footer.x + FOOTER_PAD, y, button_w, h);
    out->buttons[SAND_TITLE_EXIT] = mu_rect(out->footer.x + out->footer.w - FOOTER_PAD - button_w, y, button_w, h);
}

void
title_screen_layout(int screen_w, int screen_h, title_screen_layout_t* out) {
    out->header = mu_rect(0, 0, screen_w, HEADER_H);
    out->title_scale = largest_title_scale(screen_w - 2 * UI_MARGIN);
    out->subtitle = mu_rect(UI_MARGIN, HEADER_H + SUBTITLE_GAP, screen_w - 2 * UI_MARGIN, SUBTITLE_H);
    layout_footer(screen_w, screen_h, out);

    const int stack_h = MAIN_COUNT * MAIN_H + (MAIN_COUNT - 1) * MAIN_GAP;
    const int room_top = out->subtitle.y + out->subtitle.h;
    const int top = room_top + (out->footer.y - room_top - stack_h) / 2;
    const int w = min_int(screen_w - 2 * UI_MARGIN, MAIN_W_MAX);
    for (int i = 0; i < MAIN_COUNT; i++) {
        out->buttons[SAND_TITLE_START + i] = ui_centered_rect(screen_w, w, MAIN_H, top + i * (MAIN_H + MAIN_GAP));
    }
}

sand_title_button_t
title_screen_draw(mu_Context* ctx) {
    ui_set_text_style(UI_TEXT_PLAIN);

    title_screen_layout_t lay;
    title_screen_layout(ui_width(), ui_height(), &lay);

    if (!ui_begin_screen(ctx, "Sand Title", MU_OPT_NOTITLE | MU_OPT_NORESIZE | MU_OPT_NOCLOSE | MU_OPT_NOFRAME)) {
        return SAND_TITLE_NONE;
    }

    const ui_theme_t* theme = &sand_ui_theme;
    ui_header_bar(ctx, lay.header, TITLE_SCREEN_TITLE, lay.title_scale, theme);
    ui_text_in(ctx, lay.subtitle, TITLE_SCREEN_SUBTITLE, theme->accent, theme->text_scale, UI_ALIGN_CENTRE);
    ui_panel(ctx, lay.footer, theme);

    sand_title_button_t hit = SAND_TITLE_NONE;
    for (int i = 0; i < SAND_TITLE_BUTTON_COUNT; i++) {
        const ui_widget_button_t button = {
            .icon = &icon_sand_table[ICONS[i]],
            .icon_rows = icon_sand_rows,
            .label = LABELS[i],
            .enabled = title_screen_button_enabled((sand_title_button_t)i),
        };
        if (ui_icon_button(ctx, IDS[i], lay.buttons[i], &button, theme)) {
            hit = (sand_title_button_t)i;
        }
    }

    mu_end_window(ctx);
    return hit;
}
