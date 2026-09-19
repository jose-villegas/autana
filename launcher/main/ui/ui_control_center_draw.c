/*
 * ui_control_center_draw - the Control Center's own widgets, placed at the
 * rects of a control_center_layout_t. No framebuffer access, so it links and
 * runs on a host the same way ui_launcher_draw.c does; ui_control_center.c
 * owns the frame and the dimmed backdrop.
 */

#include "ui/ui_control_center.h"

#include <string.h>

#include "gfx/gfx_font_roles.h"
#include "gfx/icons_system.h"
#include "ui/ui.h"
#include "ui/ui_slider.h"

static const mu_Color color_panel = {0x10, 0x2C, 0x3B, 255};
static const mu_Color color_panel_focus = {0x13, 0x3B, 0x4B, 255};
static const mu_Color color_border = {0x2B, 0x55, 0x68, 255};
static const mu_Color color_text = {0xE9, 0xF4, 0xF7, 255};
static const mu_Color color_muted = {0x83, 0xA7, 0xB5, 255};
static const mu_Color color_cyan = {0x59, 0xE5, 0xEB, 255};
static const mu_Color color_purple = {0xA7, 0x81, 0xFF, 255};
static const mu_Color color_orange = {0xFF, 0xB4, 0x4F, 255};

static mu_Rect
control_rect(const control_center_layout_t* layout, control_center_element_id_t element) {
    const control_center_layout_rect_t* rect = &layout->rects[element];
    return mu_rect(rect->x, rect->y, rect->width, rect->height);
}

const control_center_layout_t*
ui_control_center_baked_layout(void) {
    if (ui_width() == control_center_layout_landscape.canvas_width
        && ui_height() == control_center_layout_landscape.canvas_height) {
        return &control_center_layout_landscape;
    }
    return &control_center_layout_portrait;
}

static void
draw_text(mu_Context* ctx, mu_Rect rect, const char* text, mu_Color color) {
    mu_draw_text(ctx, ctx->style->font, text, -1, mu_vec2(rect.x, rect.y), color);
}

static void
draw_panel(mu_Context* ctx, mu_Rect panel, mu_Color face, mu_Color border) {
    mu_draw_rect(ctx, panel, face);
    mu_draw_rect(ctx, mu_rect(panel.x, panel.y, panel.w, 2), border);
    mu_draw_rect(ctx, mu_rect(panel.x, panel.y + panel.h - 2, panel.w, 2), border);
    mu_draw_rect(ctx, mu_rect(panel.x, panel.y, 2, panel.h), border);
    mu_draw_rect(ctx, mu_rect(panel.x + panel.w - 2, panel.y, 2, panel.h), border);
}

static void
draw_connectivity_card(mu_Context* ctx, mu_Rect panel, const char* title, const char* status, mu_Color accent,
                       bool active, icon_system_id_t icon) {
    const mu_Id id = mu_get_id(ctx, title, (int)strlen(title));
    mu_update_control(ctx, id, panel, 0);
    const bool highlighted = active || ctx->focus == id || ctx->hover == id;
    draw_panel(ctx, panel, highlighted ? color_panel_focus : color_panel, highlighted ? accent : color_border);
    ui_draw_icon(ctx, mu_rect(panel.x + 12, panel.y + (panel.h - 24) / 2, 24, 24), &icon_system_table[icon],
                 icon_system_rows, accent);
    draw_text(ctx, mu_rect(panel.x + 46, panel.y + panel.h / 2 - 18, panel.w - 58, 16), title, color_text);
    draw_text(ctx, mu_rect(panel.x + 46, panel.y + panel.h / 2 + 4, panel.w - 58, 16), status, color_muted);
    mu_draw_rect(ctx, mu_rect(panel.x + panel.w - 16, panel.y + 12, 6, 6), active ? accent : color_muted);
}

static void
draw_slider(mu_Context* ctx, mu_Rect panel, const char* label, int* value, mu_Color accent) {
    const mu_Id id = mu_get_id(ctx, &value, sizeof(value));
    const mu_Rect touch = panel;
    const mu_Rect track = mu_rect(panel.x + 10, panel.y + 34, panel.w - 20, 6);
    mu_update_control(ctx, id, touch, MU_OPT_HOLDFOCUS);
    if (ctx->focus == id && (ctx->mouse_down | ctx->mouse_pressed) == MU_MOUSE_LEFT) {
        *value = ui_slider_value_at_x(track, 0, 100, 14, 5, ctx->mouse_pos.x);
    }
    draw_panel(ctx, panel, color_panel, color_border);
    draw_text(ctx, mu_rect(panel.x + 10, panel.y + 7, panel.w - 20, 16), label, color_muted);
    mu_draw_rect(ctx, track, color_border);
    mu_draw_rect(ctx, ui_slider_fill_rect(track, 0, 100, *value, 14), accent);
    const int knob_x = ui_slider_knob_center_x(track, 0, 100, *value, 14);
    mu_draw_rect(ctx, mu_rect(knob_x - 7, track.y - 4, 14, 14), color_text);
}

static void
draw_notification(mu_Context* ctx, mu_Rect panel, const char* title, const char* status, mu_Color accent,
                  icon_system_id_t icon) {
    const mu_Id id = mu_get_id(ctx, title, (int)strlen(title));
    mu_update_control(ctx, id, panel, 0);
    draw_panel(ctx, panel, ctx->focus == id || ctx->hover == id ? color_panel_focus : color_panel, color_border);
    const mu_Rect badge = mu_rect(panel.x + 12, panel.y + (panel.h - 24) / 2, 24, 24);
    mu_draw_rect(ctx, badge, accent);
    ui_draw_icon(ctx, badge, &icon_system_table[icon], icon_system_rows, color_text);
    draw_text(ctx, mu_rect(panel.x + 48, panel.y + panel.h / 2 - 17, panel.w - 82, 16), title, color_text);
    draw_text(ctx, mu_rect(panel.x + 48, panel.y + panel.h / 2 + 3, panel.w - 82, 16), status, color_muted);
    ui_draw_icon(ctx, mu_rect(panel.x + panel.w - 28, panel.y + (panel.h - 16) / 2, 16, 16),
                 &icon_system_table[ICON_SYSTEM_CHEVRON_RIGHT], icon_system_rows, color_cyan);
}

void
ui_control_center_draw(mu_Context* ctx, const control_center_layout_t* layout) {
    static int volume = 65;
    static int brightness = 80;

    ui_set_button_style(UI_BUTTON_BEZEL);
    ui_set_font_scaled(gfx_font_ui(), 1);

    if (ui_begin_screen(ctx, "Control Center", MU_OPT_NOTITLE | MU_OPT_NORESIZE | MU_OPT_NOCLOSE | MU_OPT_NOFRAME)) {
        draw_connectivity_card(ctx, control_rect(layout, CONTROL_CENTER_ELEMENT_WIFI), "WI-FI", "STUDIO-5G", color_cyan,
                               true, ICON_SYSTEM_WIFI);
        draw_connectivity_card(ctx, control_rect(layout, CONTROL_CENTER_ELEMENT_BLUETOOTH), "BLUETOOTH", "CONTROLLER",
                               color_purple, false, ICON_SYSTEM_GAMEPAD);
        draw_connectivity_card(ctx, control_rect(layout, CONTROL_CENTER_ELEMENT_LINK), "LINK", "USB READY",
                               color_orange, false, ICON_SYSTEM_MONITOR);

        draw_slider(ctx, control_rect(layout, CONTROL_CENTER_ELEMENT_VOLUME), "VOLUME", &volume, color_purple);
        draw_slider(ctx, control_rect(layout, CONTROL_CENTER_ELEMENT_BRIGHTNESS), "BRIGHTNESS", &brightness,
                    color_cyan);

        const mu_Rect notifications = control_rect(layout, CONTROL_CENTER_ELEMENT_NOTIFICATIONS_HEADER);
        draw_text(ctx, notifications, "NOTIFICATIONS", color_muted);
        const char* clear = "CLEAR ALL";
        draw_text(ctx,
                  mu_rect(notifications.x + notifications.w - ui_measure_text(clear), notifications.y, notifications.w,
                          notifications.h),
                  clear, color_cyan);
        draw_notification(ctx, control_rect(layout, CONTROL_CENTER_ELEMENT_LIBRARY_NOTIFICATION),
                          "LIBRARY SCAN COMPLETE", "12 GAMES READY TO PLAY", color_cyan, ICON_SYSTEM_PLUS);
        draw_notification(ctx, control_rect(layout, CONTROL_CENTER_ELEMENT_CONTROLLER_NOTIFICATION),
                          "CONTROLLER CONNECTED", "INPUT PROFILE LOADED", color_purple, ICON_SYSTEM_GAMEPAD);
        mu_end_window(ctx);
    }
    ui_set_font(gfx_font_ui());
}
