/*
 * ui_widgets - the controls microui does not have, built the way
 * docs/Building-a-Screen.md asks: mu_get_id() + mu_update_control() for the
 * hit, commands for everything drawn, and the caller deciding what a hit
 * means.
 *
 * Every widget takes a ui_theme_t rather than reading colours of its own,
 * so a screen states its look once and each widget stays the same code for
 * every screen that uses it. Labels are the UI font at the theme's scale.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "gfx/icon.h"
#include "microui.h"

typedef struct {
    mu_Color panel_face;
    mu_Color panel_edge;
    mu_Color button_face;
    mu_Color accent;      /* headings */
    mu_Color accent_face; /* the selected choice's face */
    mu_Color on_accent;   /* ink on accent_face */
    mu_Color text;        /* ink at rest */
    mu_Color caption;
    mu_Color muted; /* a disabled control's ink */
    int text_scale;
    int icon_side; /* beside a label; a tile sizes its own to fill it */
} ui_theme_t;

typedef enum { UI_ALIGN_LEFT, UI_ALIGN_CENTRE, UI_ALIGN_RIGHT } ui_align_t;

typedef struct {
    const icon_t* icon; /* NULL for a label alone */
    const uint8_t* icon_rows;
    const char* label;
    bool enabled;  /* a disabled button is drawn muted and takes no tap */
    bool selected; /* drawn on accent_face; a press only sinks the bezel */
} ui_widget_button_t;

#define UI_CHECK_ON  "[X]"
#define UI_CHECK_OFF "[ ]"

/* 0xRRGGBB, opaque: UI_RGB() as an initializer, so a theme can be const
 * data, and ui_rgb() in an expression. */
#define UI_RGB(rgb)                                                                                                    \
    {(unsigned char)(((rgb) >> 16) & 0xFF), (unsigned char)(((rgb) >> 8) & 0xFF), (unsigned char)((rgb) & 0xFF), 255}

mu_Color ui_rgb(uint32_t rgb);

void ui_panel(mu_Context* ctx, mu_Rect r, const ui_theme_t* theme);

/* A full-width bar with an edge along its bottom and `title` centred. */
void ui_header_bar(mu_Context* ctx, mu_Rect r, const char* title, int scale, const ui_theme_t* theme);

/* `str` clipped to `r`, centred vertically. */
void ui_text_in(mu_Context* ctx, mu_Rect r, const char* str, mu_Color color, int scale, ui_align_t align);

/* The icon at the left, the label centred in what remains. Returns whether
 * this frame's tap landed on it. `id` must be unique within the window. */
bool ui_icon_button(mu_Context* ctx, const char* id, mu_Rect r, const ui_widget_button_t* button,
                    const ui_theme_t* theme);

/* How wide a label may be inside a `button_w` ui_icon_button(). */
int ui_icon_button_label_width(int button_w, bool has_icon, const ui_theme_t* theme);

/* The icon above the label, for one of a row of choices; the icon takes
 * whatever square the label leaves - ui_tile_icon_rect(), where a caller
 * with no icon may draw its own picture of the choice instead. */
mu_Rect ui_tile_icon_rect(mu_Rect r, const ui_theme_t* theme);

/* `colors` as a `cols` x `rows` grid of equal cells filling `r`, row by
 * row: a sample of real colours, where an icon would only stand for one. */
void ui_swatch_grid(mu_Context* ctx, mu_Rect r, const mu_Color* colors, int cols, int rows);

bool ui_tile_button(mu_Context* ctx, const char* id, mu_Rect r, const ui_widget_button_t* button,
                    const ui_theme_t* theme);

typedef struct {
    const icon_t* icon; /* NULL for a label alone */
    const uint8_t* icon_rows;
    const char* label;
} ui_dropdown_item_t;

/* Where a dropdown's open list goes: below `anchor` when `count` rows of
 * `row_h` fit above `canvas_h - margin`, above it when they fit there, and
 * otherwise on whichever side has more room, pinned inside the margins.
 * Never taller than the canvas inside its margins; the list scrolls. */
mu_Rect ui_dropdown_list_rect(mu_Rect anchor, int count, int row_h, int canvas_h, int margin);

/* Where a list `list_h` tall opens scrolled to: `selected` centred when it
 * can be, and never past either end. */
int ui_dropdown_list_scroll(int selected, int count, int row_h, int list_h);

/* How wide the chosen item's label may be inside a `w` dropdown. */
int ui_dropdown_label_width(int w, const ui_theme_t* theme);

/* The chosen item with a chevron; a tap opens the rest as a list over the
 * screen, placed by ui_dropdown_list_rect(). Returns the index picked this
 * frame, or -1. `id` names the list's own window, unique per dropdown. */
int ui_dropdown(mu_Context* ctx, const char* id, mu_Rect r, const ui_dropdown_item_t* items, int count, int selected,
                const ui_theme_t* theme);

/* Whether dropdown `id`'s list is open. Asked from inside the same window
 * as the ui_dropdown() call, since microui scopes a name to its window. */
bool ui_dropdown_is_open(mu_Context* ctx, const char* id);

/* A row reading "[X] label" or "[ ] label" after an optional icon; a tap
 * reports, never toggles. `row->label` is the bare label. */
bool ui_check_row(mu_Context* ctx, const char* id, mu_Rect r, const ui_widget_button_t* row, bool checked,
                  const ui_theme_t* theme);
