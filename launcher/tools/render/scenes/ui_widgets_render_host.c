/*
 * ui_widgets_render_host - the shell's UI toolkit on one page per view, drawn
 * by the real code in either orientation: the themed widgets, the dropdown
 * with its list open, microui's own controls in both button styles, and a
 * small settings screen. docs/UI-Toolkit.md shows these images; the pins
 * keep them honest.
 */

#include <stdbool.h>
#include <string.h>

#include "gfx/gfx.h"
#include "gfx/icons_system.h"
#include "render_host.h"
#include "ui/ui.h"
#include "ui/ui_transform.h"
#include "ui/ui_widgets.h"

#define BACKGROUND 0x0A0C14
#define HEADER_H   40

static const ui_theme_t THEME = {
    .panel_face = UI_RGB(0x131C2E),
    .panel_edge = UI_RGB(0xE8ECF4),
    .button_face = UI_RGB(0x1B2740),
    .accent = UI_RGB(0xE0A63C),
    .accent_face = UI_RGB(0xE0A63C),
    .on_accent = UI_RGB(0x2A1A06),
    .text = UI_RGB(0xF2F6FF),
    .caption = UI_RGB(0x8FA3C0),
    .muted = UI_RGB(0x4A5672),
    .text_scale = 2,
    .icon_side = 24,
};

typedef enum { VIEW_WIDGETS, VIEW_DROPDOWN_OPEN, VIEW_MICROUI, VIEW_SETTINGS } view_t;

static view_t view = VIEW_WIDGETS;
static ui_transform_t transform;
static int quality = 2;
static int microui_value = 6;
static int checked = 1;
static int picked_item = 1;

int
display_shell_quarter(void) {
    return 0;
}

static bool
options(int argc, char** argv) {
    static const struct {
        const char* name;
        view_t view;
    } VIEWS[] = {
        {"widgets", VIEW_WIDGETS},
        {"dropdown-open", VIEW_DROPDOWN_OPEN},
        {"microui", VIEW_MICROUI},
        {"settings", VIEW_SETTINGS},
    };

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--view") != 0 || i + 1 >= argc) {
            return false;
        }
        const char* name = argv[++i];
        bool known = false;
        for (size_t v = 0; v < sizeof VIEWS / sizeof VIEWS[0]; v++) {
            if (strcmp(name, VIEWS[v].name) == 0) {
                view = VIEWS[v].view;
                known = true;
            }
        }
        if (!known) {
            return false;
        }
    }
    return true;
}

static bool
setup(int quarter) {
    ui_init();
    transform = ui_transform_quarter_turn(quarter, GFX_WIDTH, GFX_HEIGHT);
    ui_set_transform(transform);
    return true;
}

static bool
landscape(void) {
    return ui_width() > ui_height();
}

static ui_widget_button_t
button(icon_system_id_t icon, const char* label, bool enabled, bool selected) {
    return (ui_widget_button_t){
        .icon = &icon_system_table[icon],
        .icon_rows = icon_system_rows,
        .label = label,
        .enabled = enabled,
        .selected = selected,
    };
}

static void
draw_dropdown(mu_Context* ctx, mu_Rect r) {
    static const ui_dropdown_item_t items[] = {
        {&icon_system_table[ICON_SYSTEM_HOME], icon_system_rows, "HOME"},
        {&icon_system_table[ICON_SYSTEM_WIFI], icon_system_rows, "WIFI"},
        {&icon_system_table[ICON_SYSTEM_MONITOR], icon_system_rows, "DISPLAY"},
        {&icon_system_table[ICON_SYSTEM_GAMEPAD], icon_system_rows, "GAMES"},
    };
    const int count = (int)(sizeof items / sizeof items[0]);
    const int picked = ui_dropdown(ctx, "gallery", r, items, count, picked_item, &THEME);
    if (picked >= 0) {
        picked_item = picked;
    }
}

static void
draw_slider_panel(mu_Context* ctx, mu_Rect r) {
    ui_panel(ctx, r, &THEME);
    const mu_Rect caption = mu_rect(r.x + 12, r.y + 6, r.w - 24, 24);
    ui_text_in(ctx, caption, "QUALITY", THEME.caption, 2, UI_ALIGN_LEFT);
    ui_text_in(ctx, caption, "NORMAL", THEME.text, 2, UI_ALIGN_RIGHT);
    ui_theme_slider_int(ctx, mu_rect(r.x + 12, r.y + 34, r.w - 24, UI_TAP_MIN), &quality, 0, 4, 1, &THEME);
}

static void
draw_tiles(mu_Context* ctx, mu_Rect r) {
    static const char* const IDS[] = {"info", "wifi", "off"};
    const ui_widget_button_t tiles[] = {
        button(ICON_SYSTEM_INFO, "INFO", true, false),
        button(ICON_SYSTEM_WIFI, "WIFI", true, true),
        button(ICON_SYSTEM_ALERT, "OFF", false, false),
    };
    const int w = (r.w - 16) / 3;
    for (int i = 0; i < 3; i++) {
        ui_tile_button(ctx, IDS[i], mu_rect(r.x + i * (w + 8), r.y, w, r.h), &tiles[i], &THEME);
    }
}

static void
draw_swatches(mu_Context* ctx, mu_Rect r) {
    static const mu_Color SWATCHES[] = {
        UI_RGB(0x1D2B53), UI_RGB(0x7E2553), UI_RGB(0x008751), UI_RGB(0xAB5236), UI_RGB(0x5F574F), UI_RGB(0xC2C3C7),
        UI_RGB(0xFFF1E8), UI_RGB(0xFF004D), UI_RGB(0xFFA300), UI_RGB(0xFFEC27), UI_RGB(0x00E436), UI_RGB(0x29ADFF),
        UI_RGB(0x83769C), UI_RGB(0xFF77A8), UI_RGB(0xFFCCAA), UI_RGB(0x000000),
    };
    ui_swatch_grid(ctx, r, SWATCHES, 8, 2);
}

static void
draw_icon_buttons(mu_Context* ctx, mu_Rect home_r, mu_Rect on_r) {
    const ui_widget_button_t home = button(ICON_SYSTEM_HOME, "HOME", true, false);
    const ui_widget_button_t on = button(ICON_SYSTEM_CHECK, "ON", true, true);
    ui_icon_button(ctx, "home", home_r, &home, &THEME);
    ui_icon_button(ctx, "on", on_r, &on, &THEME);
}

static void
draw_widgets(mu_Context* ctx) {
    const int w = ui_width();
    ui_header_bar(ctx, mu_rect(0, 0, w, HEADER_H), "UI WIDGETS", 2, &THEME);
    if (landscape()) {
        const int col_w = (w - 40) / 2;
        const int right_x = 24 + col_w;
        draw_slider_panel(ctx, mu_rect(16, 48, w - 32, 92));
        draw_tiles(ctx, mu_rect(16, 148, w - 32, 76));
        draw_icon_buttons(ctx, mu_rect(16, 232, col_w, UI_TAP_MIN), mu_rect(16, 296, col_w, UI_TAP_MIN));
        draw_dropdown(ctx, mu_rect(right_x, 232, col_w, UI_TAP_MIN));
        draw_swatches(ctx, mu_rect(right_x, 296, col_w, UI_TAP_MIN));
        return;
    }
    const int half = (w - 40) / 2;
    draw_slider_panel(ctx, mu_rect(16, 52, w - 32, 100));
    draw_icon_buttons(ctx, mu_rect(16, 164, half, UI_TAP_MIN), mu_rect(24 + half, 164, half, UI_TAP_MIN));
    draw_tiles(ctx, mu_rect(16, 232, w - 32, 88));
    draw_swatches(ctx, mu_rect(16, 332, w - 32, 32));
    draw_dropdown(ctx, mu_rect(16, 376, w - 32, UI_TAP_MIN));
}

static mu_Rect
open_view_dropdown(void) {
    return mu_rect(16, ui_height() - 16 - UI_TAP_MIN, ui_width() - 32, UI_TAP_MIN);
}

static void
draw_text_scales(mu_Context* ctx, int x, int y, int w) {
    ui_text_in(ctx, mu_rect(x, y, w, 16), "SCALE 1", THEME.text, 1, UI_ALIGN_LEFT);
    ui_text_in(ctx, mu_rect(x, y + 20, w, 24), "SCALE 2", THEME.text, 2, UI_ALIGN_LEFT);
    ui_text_in(ctx, mu_rect(x, y + 48, w, 32), "SCALE 3", THEME.text, 3, UI_ALIGN_LEFT);
}

static void
draw_microui(mu_Context* ctx) {
    const int w = ui_width();
    ui_header_bar(ctx, mu_rect(0, 0, w, HEADER_H), "MICROUI", 2, &THEME);
    const bool wide = landscape();
    const int col_w = wide ? (w - 48) / 2 : w - 32;
    mu_layout_set_next(ctx, mu_rect(16, 56, col_w, UI_ROW_HEIGHT), 0);
    mu_button(ctx, "FLAT");
    ui_set_button_style(UI_BUTTON_BEZEL);
    mu_layout_set_next(ctx, mu_rect(16, 132, col_w, UI_ROW_HEIGHT), 0);
    mu_button(ctx, "BEZEL");
    mu_layout_set_next(ctx, mu_rect(16, 208, col_w, UI_ROW_HEIGHT), 0);
    mu_checkbox(ctx, "CHECKBOX", &checked);
    const int right_x = wide ? 32 + col_w : 16;
    mu_layout_set_next(ctx, mu_rect(right_x, wide ? 56 : 284, col_w, UI_ROW_HEIGHT), 0);
    ui_slider_int(ctx, &microui_value, 2, 12, 2);
    draw_text_scales(ctx, right_x, wide ? 136 : 356, col_w);
}

static int volume = 5;
static int muted;
static int output;

static const ui_dropdown_item_t OUTPUTS[] = {
    {.label = "SPEAKER"},
    {.label = "HEADPHONES"},
    {.label = "BLUETOOTH"},
    {.label = "OFF"},
};

static void
draw_settings(mu_Context* ctx) {
    const int w = ui_width();
    const int row_w = w - 2 * UI_MARGIN;
    ui_header_bar(ctx, mu_rect(0, 0, w, UI_TITLE_HEIGHT), "SETTINGS", 2, &THEME);
    ui_theme_slider_int(ctx, mu_rect(UI_MARGIN, 72, row_w, UI_TAP_MIN), &volume, 0, 10, 1, &THEME);
    mu_layout_set_next(ctx, mu_rect(UI_MARGIN, 144, row_w, UI_TAP_MIN), 0);
    mu_checkbox(ctx, "MUTE", &muted);
    const int picked =
        ui_dropdown(ctx, "output", mu_rect(UI_MARGIN, 216, row_w, UI_TAP_MIN), OUTPUTS, 4, output, &THEME);
    if (picked >= 0) {
        output = picked;
    }
}

/* Four held frames: ui_pointer waits out microui's hover lag before a press. */
static input_t
open_dropdown_tap(int frame) {
    input_t in = {0};
    const int start = 2;
    const int held = 4;
    const int t = frame - start;
    if (view != VIEW_DROPDOWN_OPEN || t < 0 || t > held) {
        return in;
    }
    const mu_Rect r = open_view_dropdown();
    ui_transform_point(transform, r.x + r.w / 2, r.y + r.h / 2, &in.x, &in.y);
    in.down = t < held;
    in.pressed = t == 0;
    in.released = t == held;
    return in;
}

static void
draw(const render_frame_t* frame) {
    const input_t in = open_dropdown_tap(frame->index);
    ui_begin(&in);
    ui_set_text_style(UI_TEXT_PLAIN);
    mu_Context* ctx = ui_context();
    if (ui_begin_screen(ctx, "Gallery", MU_OPT_NOTITLE | MU_OPT_NORESIZE | MU_OPT_NOCLOSE | MU_OPT_NOFRAME)) {
        switch (view) {
            case VIEW_MICROUI: draw_microui(ctx); break;
            case VIEW_SETTINGS: draw_settings(ctx); break;
            case VIEW_DROPDOWN_OPEN:
                ui_header_bar(ctx, mu_rect(0, 0, ui_width(), HEADER_H), "DROPDOWN", 2, &THEME);
                draw_dropdown(ctx, open_view_dropdown());
                break;
            case VIEW_WIDGETS: draw_widgets(ctx); break;
        }
        mu_end_window(ctx);
    }
    ui_end(BACKGROUND);
}

const render_scene_t render_scene = {
    .name = "ui_widgets",
    .quarter = 0,
    .frames = 12,
    .dt_ms = 33,
    .options = options,
    .setup = setup,
    .draw = draw,
};
