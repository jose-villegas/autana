/* The themed widgets the face verbs are built from, private to ui/. */
#pragma once

#include "ui/controls/ui_icons.h"
#include "ui/ui_internal.h"

/* System icon `id` filling `r`, or its short text when there is no pack. */
void ui_draw_system_icon(mu_Context* ctx, mu_Rect r, ui_icon_id_t id, mu_Color color);

/* Forgets any open dropdown list's state; ui_init() calls it. */
void ui_widgets_reset(void);

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

/* ui_theme_t `t` as an ui_mu_theme_t literal: assigned straight into its
 * destination, so ui_theme() builds the face's theme in place each frame. */
#define UI_THEME_FACE_COLOR(t, member) .member = ui_rgb((t)->member)
#define UI_THEME_FROM_FACE(t)                                                                                          \
    ((ui_mu_theme_t){UI_THEME_FACE_COLOR(t, panel_face), UI_THEME_FACE_COLOR(t, panel_edge),                           \
                     UI_THEME_FACE_COLOR(t, button_face), UI_THEME_FACE_COLOR(t, accent),                              \
                     UI_THEME_FACE_COLOR(t, accent_face), UI_THEME_FACE_COLOR(t, on_accent),                           \
                     UI_THEME_FACE_COLOR(t, text), UI_THEME_FACE_COLOR(t, caption), UI_THEME_FACE_COLOR(t, muted),     \
                     .text_scale = (t)->text_scale, .icon_side = (t)->icon_side})

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
 * whatever square the label leaves: ui_mu_tile_icon_rect(). */
mu_Rect ui_mu_tile_icon_rect(mu_Rect r, const ui_mu_theme_t* theme);

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
int ui_dropdown_label_width(int w, bool has_icons, const ui_mu_theme_t* theme);

/* The chosen item with a chevron; a tap opens the rest as a list over the
 * screen, placed by ui_dropdown_list_rect(). Returns the index picked this
 * frame, or -1; the list closes UI_DROPDOWN_CLOSE_FRAMES frames later.
 * `names` holds at least one entry, and `id` must be unique per dropdown;
 * its list's window is named after it. */
int ui_mu_dropdown(mu_Context* ctx, const char* id, mu_Rect r, const char* const* names,
                   const draw_image_t* const* icons, int count, int selected, const ui_mu_theme_t* theme);

/* Whether dropdown `id`'s list is open. Asked from inside the same window
 * as the ui_mu_dropdown() call, since microui scopes a name to its window. */
bool ui_dropdown_is_open(mu_Context* ctx, const char* id);

bool ui_icon_button_id(mu_Context* ctx, mu_Id id, mu_Rect r, const ui_widget_button_t* button, int label_len,
                       const ui_mu_theme_t* theme);
bool ui_tile_button_id(mu_Context* ctx, mu_Id id, mu_Rect r, const ui_widget_button_t* button, int label_len,
                       const ui_mu_theme_t* theme);
bool ui_theme_slider_int_id(mu_Context* ctx, mu_Id id, mu_Rect r, int* value, int lo, int hi, int step, bool enabled,
                            const ui_mu_theme_t* theme);
int ui_dropdown_id(mu_Context* ctx, mu_Id mid, const char* id, mu_Rect r, const char* const* names,
                   const draw_image_t* const* icons, int count, int selected, bool enabled, const ui_mu_theme_t* theme);

/* The dropdown list open this frame, or NULL: at most one is. */
const mu_Container* ui_dropdown_open_list(void);

bool ui_toggle_id(mu_Context* ctx, mu_Id mid, mu_Rect r, const char* label, int label_len, bool* on, bool enabled,
                  const ui_mu_theme_t* theme);
