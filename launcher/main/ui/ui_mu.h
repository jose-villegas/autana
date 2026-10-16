/* Transitional API for screens still written against microui.
 * Include ui.h alone for screens using the plain-value face. */
#pragma once

#include "ui/ui.h"

/* The microui context, for building the UI between ui_begin and ui_end. */
mu_Context* ui_context(void);

static inline mu_Color
ui_rgb(uint32_t rgb) {
    return (mu_Color)UI_RGB(rgb);
}

/* Opens a full-screen window sized to THIS FRAME's canvas, fixing a
 * microui mismatch: mu_begin_window_ex() only trusts its rect the first
 * time a window (by title) opens, right for a desktop manager
 * remembering a drag, wrong here, where every window must always BE
 * (0, 0, ui_width(), ui_height()), which changes at runtime as the
 * canvas rotates. Uncorrected, a rect pins to the first-open
 * orientation, and a bigger later orientation leaves part of the screen
 * uncleared, showing a previous app's frame. */
int ui_begin_screen(mu_Context* ctx, const char* title, int opt);

/*
 * FILL content (a banner, a status strip) has no natural width of its own
 * and spans whatever the canvas currently is, which mu_layout_row() with a
 * -1 column already does. FIXED content (a few large tap targets sized to
 * what they need to say) should keep that width and centre instead.
 *
 * ui_centered_rect() is the shared primitive for the fixed case. `canvas_w`
 * is a parameter rather than an internal ui_width() call, which is what
 * keeps it host-testable without reaching the framebuffer.
 */

/* A rect `w` wide, `h` tall, horizontally centred within a canvas
 * `canvas_w` wide, at vertical position `y`. Does not clamp `w` to
 * `canvas_w`: a `w` that exceeds `canvas_w` yields a negative x, which
 * is a caller bug (a button wider than the screen it is centred on)
 * rather than something to paper over silently here. Clamp at the call
 * site if `w` might ever exceed `canvas_w`. */
static inline mu_Rect
ui_centered_rect(int canvas_w, int w, int h, int y) {
    return (mu_Rect){(canvas_w - w) / 2, y, w, h};
}

/* `icon` filling `r` in `color`, its runs streamed by icon_walk_blocks()
 * rather than collected, so an icon's run count does not bound artwork. A
 * NULL `icon` (no pack holds it) draws `text` centred in `r` instead, in the
 * current font; a control with a label beside its icon passes NULL `text`
 * and keeps the label alone. */
void ui_draw_icon(mu_Context* ctx, mu_Rect r, const draw_image_t* icon, const char* text, mu_Color color);

/* An integer-valued slider over the next layout row, shaped like
 * mu_slider_ex(), but integer: that one's float/"%.2f" thumb is the wrong
 * shape for a "06 PX" control. Writes through `value`, returns whether it
 * changed this frame. */
bool ui_slider_int(mu_Context* ctx, int* value, int lo, int hi, int step);

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
    int icon_side;      /* beside a label; a tile sizes its own to fill it */
    bool native_toggle; /* retain the style's checkbox frame palette */
} ui_mu_theme_t;

ui_mu_theme_t ui_theme_from_style(const mu_Style* style);

typedef enum { UI_ALIGN_LEFT, UI_ALIGN_CENTRE, UI_ALIGN_RIGHT } ui_align_t;

typedef struct {
    const draw_image_t* icon; /* NULL for a label alone */
    const char* label;
    bool enabled;  /* a disabled button is drawn muted and takes no tap */
    bool selected; /* drawn on accent_face; a press only sinks the bezel */
} ui_widget_button_t;

void ui_mu_panel(mu_Context* ctx, mu_Rect r, const ui_mu_theme_t* theme);

/* A full-width bar with an edge along its bottom and `title` centred. */
void ui_header_bar(mu_Context* ctx, mu_Rect r, const char* title, int scale, const ui_mu_theme_t* theme);

/* `str` clipped to `r`, centred vertically. */
void ui_text_in(mu_Context* ctx, mu_Rect r, const char* str, mu_Color color, int scale, ui_align_t align);

/* The icon at the left, the label centred in what remains. Returns whether
 * this frame's tap landed on it. `id` must be unique within the window. */
bool ui_icon_button(mu_Context* ctx, const char* id, mu_Rect r, const ui_widget_button_t* button,
                    const ui_mu_theme_t* theme);

/* ui_slider_int() at `r`: the filled part on accent_face, the knob in the
 * theme's text colour. */
bool ui_theme_slider_int(mu_Context* ctx, mu_Rect r, int* value, int lo, int hi, int step, const ui_mu_theme_t* theme);

/* How wide a label may be inside a `button_w` ui_icon_button(). */
int ui_icon_button_label_width(int button_w, bool has_icon, const ui_mu_theme_t* theme);

/* The icon above the label, for one of a row of choices; the icon takes
 * whatever square the label leaves: ui_tile_icon_rect(), where a caller
 * with no icon may draw its own picture of the choice instead. */
mu_Rect ui_tile_icon_rect(mu_Rect r, const ui_mu_theme_t* theme);

/* `colors` as a `cols` x `rows` grid of equal cells filling `r`, row by
 * row. */
void ui_swatch_grid(mu_Context* ctx, mu_Rect r, const uint32_t* colors, int cols, int rows);

bool ui_tile_button(mu_Context* ctx, const char* id, mu_Rect r, const ui_widget_button_t* button,
                    const ui_mu_theme_t* theme);

/* List rows fit below the anchor first, then above. Otherwise the opaque
 * list overlays the canvas, including its anchor, between the margins.
 * It uses the rows' full height when possible and scrolls only beyond the
 * canvas height minus its margins. */
mu_Rect ui_dropdown_list_rect(mu_Rect anchor, int count, int row_h, int canvas_h, int margin);

/* Where a list `list_h` tall opens scrolled to: `selected` centred when it
 * can be, and never past either end. */
int ui_dropdown_list_scroll(int selected, int count, int row_h, int list_h);

/* How wide the chosen item's label may be inside a `w` dropdown. */
int ui_dropdown_label_width(int w, const ui_mu_theme_t* theme);

/* The chosen item with a chevron; a tap opens the rest as a list over the
 * screen, placed by ui_dropdown_list_rect(). Returns the index picked this
 * frame, or -1; the list closes UI_DROPDOWN_CLOSE_FRAMES frames later.
 * `names` holds at least one entry, and `id` must be unique per dropdown;
 * its list's window is named after it. */
int ui_mu_dropdown(mu_Context* ctx, const char* id, mu_Rect r, const char* const* names,
                   const draw_image_t* const* icons, int count, int selected, const ui_mu_theme_t* theme);

/* How many frames a list stays up after its pick, once the finger lifts. */
#define UI_DROPDOWN_CLOSE_FRAMES 4

/* Whether dropdown `id`'s list is open. Asked from inside the same window
 * as the ui_mu_dropdown() call, since microui scopes a name to its window. */
bool ui_dropdown_is_open(mu_Context* ctx, const char* id);
