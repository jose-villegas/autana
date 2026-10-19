/*
 * ui_widgets_render_host, the shell's UI toolkit on one page per view, drawn
 * by the real code in either orientation: the themed widgets, the dropdown
 * with its list open, the plain controls in both button styles, and a small
 * settings screen. docs/UI-Toolkit.md shows these images; the pins
 * keep them honest.
 */

#include <stdbool.h>
#include <string.h>

#include "display/display.h"
#include "gfx/gfx.h"
#include "render_host.h"
#include "ui/controls/ui_icons.h"
#include "ui/layout/ui_transform.h"
#include "ui/ui.h"

#define BACKGROUND 0x0A0C14
#define HEADER_H   40

static const ui_theme_t THEME = {
    .panel_face = 0x131C2E,
    .panel_edge = 0xE8ECF4,
    .button_face = 0x1B2740,
    .accent = 0xE0A63C,
    .accent_face = 0xE0A63C,
    .on_accent = 0x2A1A06,
    .text = 0xF2F6FF,
    .caption = 0x8FA3C0,
    .muted = 0x4A5672,
    .text_scale = 2,
    .icon_side = 24,
};

typedef enum { VIEW_WIDGETS, VIEW_DROPDOWN_OPEN, VIEW_CONTROLS, VIEW_SETTINGS } view_t;

static view_t view = VIEW_WIDGETS;
static affine2x_t transform;
static int quality = 2;
static int control_value = 6;
static bool checked = true;
static int picked_item = 1;

int
display_quarter_now(void) {
    return DISPLAY_PORTRAIT;
}

static bool
options(int argc, char** argv) {
    static const struct {
        const char* name;
        view_t view;
    } VIEWS[] = {
        {"widgets", VIEW_WIDGETS},
        {"dropdown-open", VIEW_DROPDOWN_OPEN},
        {"controls", VIEW_CONTROLS},
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

static recti_t
rect(int x, int y, int w, int h) {
    return (recti_t){x, y, w, h};
}

static void
draw_dropdown(recti_t r) {
    static const char* const ITEMS[] = {"HOME", "WIFI", "DISPLAY", "GAMES"};
    const draw_image_t* const icons[] = {ui_icon_image(UI_ICON_HOME), ui_icon_image(UI_ICON_WIFI),
                                         ui_icon_image(UI_ICON_MONITOR), ui_icon_image(UI_ICON_GAMEPAD)};
    ui_dropdown_icons(r, "gallery", &picked_item, ITEMS, icons, (int)(sizeof ITEMS / sizeof ITEMS[0]));
}

static void
draw_slider_panel(recti_t r) {
    ui_panel(r);
    const recti_t caption = rect(r.x + 12, r.y + 6, r.w - 24, 24);
    ui_caption(caption, "QUALITY");
    ui_text_right(caption, "NORMAL");
    ui_slider(rect(r.x + 12, r.y + 34, r.w - 24, UI_TAP_MIN), "quality", &quality, 0, 4);
}

static void
draw_tiles(recti_t r) {
    const int w = (r.w - 16) / 3;
    ui_tile_icon(rect(r.x, r.y, w, r.h), "INFO", ui_icon_image(UI_ICON_INFO), false);
    ui_tile_icon(rect(r.x + w + 8, r.y, w, r.h), "WIFI", ui_icon_image(UI_ICON_WIFI), true);
    ui_disabled_begin(true);
    ui_tile_icon(rect(r.x + 2 * (w + 8), r.y, w, r.h), "OFF", ui_icon_image(UI_ICON_ALERT), false);
    ui_disabled_end();
}

static void
draw_swatches(recti_t r) {
    static const uint32_t SWATCHES[] = {
        0x1D2B53, 0x7E2553, 0x008751, 0xAB5236, 0x5F574F, 0xC2C3C7, 0xFFF1E8, 0xFF004D,
        0xFFA300, 0xFFEC27, 0x00E436, 0x29ADFF, 0x83769C, 0xFF77A8, 0xFFCCAA, 0x000000,
    };
    ui_swatches(r, SWATCHES, 8, 2);
}

static void
draw_icon_buttons(recti_t home_r, recti_t on_r) {
    ui_button_icon(home_r, "HOME", ui_icon_image(UI_ICON_HOME));
    ui_color_begin(UI_COLOR_FACE, THEME.accent_face);
    ui_color_begin(UI_COLOR_TEXT, THEME.on_accent);
    ui_button_icon(on_r, "ON", ui_icon_image(UI_ICON_CHECK));
    ui_color_end();
    ui_color_end();
}

static void
draw_widgets(void) {
    const int w = ui_width();
    ui_header(rect(0, 0, w, HEADER_H), "UI WIDGETS");
    if (landscape()) {
        const int col_w = (w - 40) / 2;
        const int right_x = 24 + col_w;
        draw_slider_panel(rect(16, 48, w - 32, 92));
        draw_tiles(rect(16, 148, w - 32, 76));
        draw_icon_buttons(rect(16, 232, col_w, UI_TAP_MIN), rect(16, 296, col_w, UI_TAP_MIN));
        draw_dropdown(rect(right_x, 232, col_w, UI_TAP_MIN));
        draw_swatches(rect(right_x, 296, col_w, UI_TAP_MIN));
        return;
    }
    const int half = (w - 40) / 2;
    draw_slider_panel(rect(16, 52, w - 32, 100));
    draw_icon_buttons(rect(16, 164, half, UI_TAP_MIN), rect(24 + half, 164, half, UI_TAP_MIN));
    draw_tiles(rect(16, 232, w - 32, 88));
    draw_swatches(rect(16, 332, w - 32, 32));
    draw_dropdown(rect(16, 376, w - 32, UI_TAP_MIN));
}

static recti_t
open_view_dropdown(void) {
    return rect(16, ui_height() - 16 - UI_TAP_MIN, ui_width() - 32, UI_TAP_MIN);
}

static void
draw_text_scales(int x, int y, int w) {
    for (int scale = 1; scale <= 3; scale++) {
        char label[] = "SCALE 0";
        label[sizeof label - 2] = (char)('0' + scale);
        ui_scale_begin(scale);
        ui_text(rect(x, y, w, 16 + 8 * (scale - 1)), label);
        ui_scale_end();
        y += 20 + 8 * (scale - 1);
    }
}

static void
draw_controls(void) {
    const int w = ui_width();
    ui_header(rect(0, 0, w, HEADER_H), "CONTROLS");
    const bool wide = landscape();
    const int col_w = wide ? (w - 48) / 2 : w - 32;
    ui_button(rect(16, 56, col_w, UI_ROW_HEIGHT), "FLAT");
    ui_set_button_style(UI_BUTTON_BEZEL);
    ui_button(rect(16, 132, col_w, UI_ROW_HEIGHT), "BEZEL");
    ui_toggle(rect(16, 208, col_w, UI_ROW_HEIGHT), "TOGGLE", &checked);
    const int right_x = wide ? 32 + col_w : 16;
    ui_slider(rect(right_x, wide ? 56 : 284, col_w, UI_ROW_HEIGHT), "value", &control_value, 2, 12);
    draw_text_scales(right_x, wide ? 136 : 356, col_w);
}

static int volume = 5;
static bool muted;
static int output;

static const char* const OUTPUTS[] = {
    "SPEAKER",
    "HEADPHONES",
    "BLUETOOTH",
    "OFF",
};

static void
draw_settings(void) {
    const int w = ui_width();
    const int row_w = w - 2 * UI_MARGIN;
    ui_header(rect(0, 0, w, UI_TITLE_HEIGHT), "SETTINGS");
    ui_slider(rect(UI_MARGIN, 72, row_w, UI_TAP_MIN), "volume", &volume, 0, 10);
    ui_toggle(rect(UI_MARGIN, 144, row_w, UI_TAP_MIN), "MUTE", &muted);
    ui_dropdown(rect(UI_MARGIN, 216, row_w, UI_TAP_MIN), "output", &output, OUTPUTS, 4);
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
    const recti_t r = open_view_dropdown();
    const vec2i_t mapped_pixel = affine2x_map_pixel(&transform, (vec2i_t){r.x + r.w / 2, r.y + r.h / 2});
    in.x = mapped_pixel.x;
    in.y = mapped_pixel.y;
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
    ui_theme(&THEME);
    ui_screen();
    switch (view) {
        case VIEW_CONTROLS: draw_controls(); break;
        case VIEW_SETTINGS: draw_settings(); break;
        case VIEW_DROPDOWN_OPEN:
            ui_header(rect(0, 0, ui_width(), HEADER_H), "DROPDOWN");
            draw_dropdown(open_view_dropdown());
            break;
        case VIEW_WIDGETS: draw_widgets(); break;
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
