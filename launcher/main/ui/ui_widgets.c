#include "ui/ui_widgets.h"

#include <stdio.h>
#include <string.h>

#include "gfx/gfx_font_roles.h"
#include "gfx/icons_system.h"
#include "ui/ui.h"
#include "ui/ui_style.h"

#define BUTTON_PAD      8
#define BUTTON_ICON_GAP 8
#define TILE_PAD        8
#define TILE_LABEL_GAP  4
#define CHEVRON_SIDE    24
#define LIST_GAP        4

mu_Color
ui_rgb(uint32_t rgb) {
    return mu_color((int)((rgb >> 16) & 0xFF), (int)((rgb >> 8) & 0xFF), (int)(rgb & 0xFF), 255);
}

static void
draw_spans(mu_Context* ctx, const ui_span_t* spans, int n) {
    for (int i = 0; i < n; i++) {
        mu_draw_rect(ctx, spans[i].rect, spans[i].color);
    }
}

void
ui_panel(mu_Context* ctx, mu_Rect r, const ui_theme_t* theme) {
    ui_span_t spans[UI_PANEL_MAX_SPANS];
    draw_spans(ctx, spans, ui_panel_spans(r, theme->panel_face, theme->panel_edge, spans, UI_PANEL_MAX_SPANS));
}

void
ui_header_bar(mu_Context* ctx, mu_Rect r, const char* title, int scale, const ui_theme_t* theme) {
    mu_draw_rect(ctx, r, theme->panel_face);
    mu_draw_rect(ctx, mu_rect(r.x, r.y + r.h - 2, r.w, 2), theme->panel_edge);
    ui_text_in(ctx, r, title, theme->accent, scale, UI_ALIGN_CENTRE);
}

void
ui_text_in(mu_Context* ctx, mu_Rect r, const char* str, mu_Color color, int scale, ui_align_t align) {
    ui_set_font_scaled(gfx_font_ui(), scale);
    const int tw = ui_measure_text(str);
    const int th = gfx_font_height(gfx_font_ui(), scale);
    int x = r.x;
    if (align == UI_ALIGN_CENTRE) {
        x = r.x + (r.w - tw) / 2;
    } else if (align == UI_ALIGN_RIGHT) {
        x = r.x + r.w - tw;
    }

    mu_push_clip_rect(ctx, r);
    mu_draw_text(ctx, ctx->style->font, str, -1, mu_vec2(x, r.y + (r.h - th) / 2), color);
    mu_pop_clip_rect(ctx);
}

typedef struct {
    bool pressed;
    bool clicked;
    mu_Color ink;
} control_t;

/* The hit, the face and the ink every button-shaped widget shares. */
static control_t
begin_control(mu_Context* ctx, const char* id, mu_Rect r, bool enabled, bool selected, const ui_theme_t* theme) {
    control_t c = {0};
    if (enabled) {
        const mu_Id mid = mu_get_id(ctx, id, (int)strlen(id));
        mu_update_control(ctx, mid, r, 0);
        /* Focus, not hover: on touch the pointer sits on whatever a drag
         * crosses, and only a landed press is a press. */
        c.pressed = ctx->focus == mid;
        c.clicked = ctx->mouse_pressed == MU_MOUSE_LEFT && ctx->focus == mid;
    }

    const bool on_accent = enabled && selected;
    ui_span_t spans[UI_BEZEL_MAX_SPANS];
    const mu_Color face = on_accent ? theme->accent_face : theme->button_face;
    draw_spans(ctx, spans, ui_bezel_spans(r, face, c.pressed, spans, UI_BEZEL_MAX_SPANS));

    if (!enabled) {
        c.ink = theme->muted;
    } else {
        c.ink = on_accent ? theme->on_accent : theme->text;
    }
    return c;
}

int
ui_icon_button_label_width(int button_w, bool has_icon, const ui_theme_t* theme) {
    return button_w - 2 * BUTTON_PAD - (has_icon ? theme->icon_side + BUTTON_ICON_GAP : 0);
}

/* The icon at the left and the label centred in `label_w` after it. */
static void
draw_icon_and_label(mu_Context* ctx, mu_Rect r, const icon_t* icon, const uint8_t* icon_rows, const char* label,
                    int label_w, mu_Color ink, const ui_theme_t* theme) {
    if (icon != NULL) {
        const int side = theme->icon_side;
        ui_draw_icon(ctx, mu_rect(r.x + BUTTON_PAD, r.y + (r.h - side) / 2, side, side), icon, icon_rows, ink);
    }
    const int label_x = r.x + BUTTON_PAD + (icon != NULL ? theme->icon_side + BUTTON_ICON_GAP : 0);
    ui_text_in(ctx, mu_rect(label_x, r.y, label_w, r.h), label, ink, theme->text_scale, UI_ALIGN_CENTRE);
}

bool
ui_icon_button(mu_Context* ctx, const char* id, mu_Rect r, const ui_widget_button_t* button, const ui_theme_t* theme) {
    const control_t c = begin_control(ctx, id, r, button->enabled, button->selected, theme);
    const int label_w = ui_icon_button_label_width(r.w, button->icon != NULL, theme);
    draw_icon_and_label(ctx, r, button->icon, button->icon_rows, button->label, label_w, c.ink, theme);
    return c.clicked;
}

mu_Rect
ui_dropdown_list_rect(mu_Rect anchor, int count, int row_h, int canvas_h, int margin) {
    const int h = count * row_h;
    const int below = anchor.y + anchor.h + LIST_GAP;
    const int room_below = canvas_h - margin - below;
    const int room_above = anchor.y - LIST_GAP - margin;

    if (h <= room_below) {
        return mu_rect(anchor.x, below, anchor.w, h);
    }
    if (h <= room_above) {
        return mu_rect(anchor.x, anchor.y - LIST_GAP - h, anchor.w, h);
    }
    if (room_below >= room_above) {
        return mu_rect(anchor.x, below, anchor.w, room_below);
    }
    return mu_rect(anchor.x, margin, anchor.w, room_above);
}

int
ui_dropdown_list_scroll(int selected, int count, int row_h, int list_h) {
    const int max_scroll = count * row_h - list_h;
    int scroll = selected * row_h - (list_h - row_h) / 2;
    if (scroll > max_scroll) {
        scroll = max_scroll;
    }
    return scroll < 0 ? 0 : scroll;
}

int
ui_dropdown_label_width(int w, const ui_theme_t* theme) {
    return ui_icon_button_label_width(w, true, theme) - CHEVRON_SIDE - BUTTON_ICON_GAP;
}

/* The list's window, or NULL before its first opening. Not
 * mu_get_container(): that creates a missing container already open. */
static mu_Container*
find_list(mu_Context* ctx, const char* list_id) {
    const mu_Id id = mu_get_id(ctx, list_id, (int)strlen(list_id));
    const int idx = mu_pool_get(ctx, ctx->container_pool, MU_CONTAINERPOOL_SIZE, id);
    return idx >= 0 ? &ctx->containers[idx] : NULL;
}

static void
list_name(const char* id, char* out, size_t len) {
    snprintf(out, len, "%s list", id);
}

/* A just-opened list's scroll, re-applied until the list has a measured
 * content height: before that microui clamps any scroll back to 0. */
static mu_Container* opening_list;
static int opening_scroll;

/* A picked list stays up UI_DROPDOWN_CLOSE_FRAMES frames after the finger
 * lifts, so the row it landed on is seen taking the pick. */
static mu_Container* closing_list;
static int closing_frames;

static int
draw_dropdown_list(mu_Context* ctx, const char* id, mu_Rect anchor, const ui_dropdown_item_t* items, int count,
                   int selected, const ui_theme_t* theme) {
    const mu_Rect list = ui_dropdown_list_rect(anchor, count, anchor.h, ui_height(), UI_MARGIN);
    mu_Container* cnt = find_list(ctx, id);
    cnt->rect = list;
    if (cnt == closing_list && !ctx->mouse_down && --closing_frames <= 0) {
        closing_list = NULL;
        cnt->open = 0;
        return -1;
    }
    if (cnt == opening_list) {
        cnt->scroll.y = opening_scroll;
        if (cnt->content_size.y > 0) {
            opening_list = NULL;
        }
    }

    /* Rows flush with the list's edges: padding would count toward the
     * content height, and a list that fits would then scroll. */
    const int saved_padding = ctx->style->padding;
    ctx->style->padding = 0;
    const int opt = MU_OPT_POPUP | MU_OPT_NORESIZE | MU_OPT_NOTITLE | MU_OPT_NOFRAME | MU_OPT_CLOSED;
    if (!mu_begin_window_ex(ctx, id, list, opt)) {
        ctx->style->padding = saved_padding;
        return -1;
    }
    const int row_w = cnt->body.w;
    int picked = -1;
    for (int i = 0; i < count; i++) {
        char row_id[24];
        snprintf(row_id, sizeof row_id, "item %d", i);
        const ui_widget_button_t row = {
            .icon = items[i].icon,
            .icon_rows = items[i].icon_rows,
            .label = items[i].label,
            .enabled = true,
            .selected = i == selected,
        };
        mu_layout_set_next(ctx, mu_rect(0, i * anchor.h, row_w, anchor.h), 1);
        if (ui_icon_button(ctx, row_id, mu_layout_next(ctx), &row, theme)) {
            picked = i;
            closing_list = cnt;
            closing_frames = UI_DROPDOWN_CLOSE_FRAMES;
        }
    }
    mu_end_window(ctx);
    ctx->style->padding = saved_padding;
    return picked;
}

bool
ui_dropdown_is_open(mu_Context* ctx, const char* id) {
    char list_id[48];
    list_name(id, list_id, sizeof list_id);
    const mu_Container* list = find_list(ctx, list_id);
    return list != NULL && list->open;
}

int
ui_dropdown(mu_Context* ctx, const char* id, mu_Rect r, const ui_dropdown_item_t* items, int count, int selected,
            const ui_theme_t* theme) {
    char list_id[48];
    list_name(id, list_id, sizeof list_id);
    const bool was_open = ui_dropdown_is_open(ctx, id);
    const control_t c = begin_control(ctx, id, r, true, false, theme);
    const ui_dropdown_item_t* chosen = &items[selected];
    draw_icon_and_label(ctx, r, chosen->icon, chosen->icon_rows, chosen->label, ui_dropdown_label_width(r.w, theme),
                        c.ink, theme);

    /* The chevron is also what repaints the screen under a closing list:
     * nothing redraws the rect a closed window leaves, but the flip changes
     * this canvas, and a changed canvas is cleared and redrawn whole. */
    const icon_system_id_t chevron = was_open ? ICON_SYSTEM_CHEVRON_UP : ICON_SYSTEM_CHEVRON_DOWN;
    const mu_Rect chevron_r = {r.x + r.w - BUTTON_PAD - CHEVRON_SIDE, r.y + (r.h - CHEVRON_SIDE) / 2, CHEVRON_SIDE,
                               CHEVRON_SIDE};
    ui_draw_icon(ctx, chevron_r, &icon_system_table[chevron], icon_system_rows, c.ink);

    if (c.clicked && !was_open) {
        mu_open_popup(ctx, list_id);
        const mu_Rect list = ui_dropdown_list_rect(r, count, r.h, ui_height(), UI_MARGIN);
        opening_list = find_list(ctx, list_id);
        if (closing_list == opening_list) {
            closing_list = NULL;
        }
        opening_scroll = ui_dropdown_list_scroll(selected, count, r.h, list.h);
    }
    if (!ui_dropdown_is_open(ctx, id)) {
        return -1;
    }
    return draw_dropdown_list(ctx, list_id, r, items, count, selected, theme);
}

mu_Rect
ui_tile_icon_rect(mu_Rect r, const ui_theme_t* theme) {
    const int label_h = gfx_font_height(gfx_font_ui(), theme->text_scale);
    int side = r.h - 2 * TILE_PAD - label_h - TILE_LABEL_GAP;
    if (side > r.w - 2 * TILE_PAD) {
        side = r.w - 2 * TILE_PAD;
    }
    return mu_rect(r.x + (r.w - side) / 2, r.y + TILE_PAD, side, side);
}

void
ui_swatch_grid(mu_Context* ctx, mu_Rect r, const mu_Color* colors, int cols, int rows) {
    for (int row = 0; row < rows; row++) {
        const int y0 = r.y + row * r.h / rows;
        const int y1 = r.y + (row + 1) * r.h / rows;
        for (int col = 0; col < cols; col++) {
            const int x0 = r.x + col * r.w / cols;
            const int x1 = r.x + (col + 1) * r.w / cols;
            mu_draw_rect(ctx, mu_rect(x0, y0, x1 - x0, y1 - y0), colors[row * cols + col]);
        }
    }
}

bool
ui_tile_button(mu_Context* ctx, const char* id, mu_Rect r, const ui_widget_button_t* button, const ui_theme_t* theme) {
    const control_t c = begin_control(ctx, id, r, button->enabled, button->selected, theme);

    const mu_Rect icon = ui_tile_icon_rect(r, theme);
    if (button->icon != NULL) {
        ui_draw_icon(ctx, icon, button->icon, button->icon_rows, c.ink);
    }
    const int label_h = gfx_font_height(gfx_font_ui(), theme->text_scale);
    const mu_Rect label = {r.x + TILE_PAD, icon.y + icon.h + TILE_LABEL_GAP, r.w - 2 * TILE_PAD, label_h};
    ui_text_in(ctx, label, button->label, c.ink, theme->text_scale, UI_ALIGN_CENTRE);
    return c.clicked;
}

bool
ui_check_row(mu_Context* ctx, const char* id, mu_Rect r, const ui_widget_button_t* row, bool checked,
             const ui_theme_t* theme) {
    char text[64];
    snprintf(text, sizeof text, "%s %s", checked ? UI_CHECK_ON : UI_CHECK_OFF, row->label);
    ui_widget_button_t marked = *row;
    marked.label = text;
    return ui_icon_button(ctx, id, r, &marked, theme);
}
