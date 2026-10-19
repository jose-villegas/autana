/* Private UI state and backend entry points shared by the frame builder,
 * face and painter. Screens use ui.h. */
#pragma once

#include <stdbool.h>

#include "profile/frame_cost.h"

#if CONFIG_LAUNCHER_SELFTEST_SCOPE_PERF
#define UI_FACE_COST_BEGIN(mark)     FRAME_COST_BEGIN(mark)
#define UI_FACE_COST_END(mark, name) FRAME_COST_END(mark, name)
#else
#define UI_FACE_COST_BEGIN(mark)     ((void)0)
#define UI_FACE_COST_END(mark, name) ((void)0)
#endif

#include "draw/draw_font.h"
#include "math/geometry/recti.h"
#include "microui.h"
#include "ui/controls/ui_style.h"
#include "ui/input/ui_pointer.h"
#include "ui/layout/ui_transform.h"
#include "ui/ui.h"
#include "ui/ui_canvas_marks.h"

static inline recti_t
ui_recti_from_mu(mu_Rect r) {
    return (recti_t){r.x, r.y, r.w, r.h};
}

static inline mu_Rect
ui_mu_from_recti(recti_t r) {
    return (mu_Rect){r.x, r.y, r.w, r.h};
}

/* ui_transform_rect() on a microui rect. */
static inline mu_Rect
ui_transform_mu_rect(affine2x_t t, mu_Rect r) {
    return ui_mu_from_recti(ui_transform_rect(t, ui_recti_from_mu(r)));
}

/* What a mu_Font actually points at; see ui_build.c's ui_set_font() comment
 * for why the scale rides inside the font rather than a separate
 * render-time setting. Shared because ui.c's draw_command() and
 * command_row_range() both resolve a command's font the same way. */
typedef struct {
    const draw_font_t* font;
    int scale;
} ui_font_scaled_t;

/* The one shell-wide microui context; see ui.h's own top comment for why
 * there is exactly one. Defined in ui_build.c; ui.c paints whatever frame it
 * holds. */
extern mu_Context ui_ctx;

/* The measured options/menu peak has a quarter-capacity margin for UI growth. */
#define UI_BAND_BIN_MEASURED_PEAK  203
#define UI_BAND_BIN_MARGIN_DIVISOR 4
#define UI_BAND_BIN_MAX                                                                                                \
    (UI_BAND_BIN_MEASURED_PEAK                                                                                         \
     + ((UI_BAND_BIN_MEASURED_PEAK + UI_BAND_BIN_MARGIN_DIVISOR - 1) / UI_BAND_BIN_MARGIN_DIVISOR))

/* One boot-lifetime PSRAM bin; allocation failure uses full-band replay. */
void ui_band_init(void);

/* Set by ui_invalidate()/ui_init() (ui_build.c), read and cleared by
 * ui_end()/ui_end_for_bands() (ui.c); see ui.h's ui_invalidate() comment. */
extern bool ui_invalidated;

/* The style MU_COMMAND_TEXT is drawn in, set by ui_set_text_style()
 * (ui_build.c), read by draw_command() (ui.c). */
extern ui_text_style_t ui_text_style;

/* Touch-to-mouse state, fed by ui_begin() (ui_build.c). ui.c's ui_end()/
 * ui_end_for_bands() report back whether this frame's pointer sits over a
 * scrollable container, the one thing painting learns that building does not
 * already know. */
extern ui_pointer_t ui_pointer_state;

/* Called by ui.c's ui_end*() right after mu_end(): reports the frame back to
 * the pointer, or not at all for a ui_begin(NULL) frame. */
void ui_end_pointer_frame(void);

/* Zeroed by ui_init() (ui_build.c) even though only ui.c's repaint-skip
 * (mark_changed_canvases()/repaint_marked_canvases()) ever reads or writes
 * it afterward; a canvas hash is meaningless until a frame has painted, but
 * "meaningless" has to start as a known value, not whatever the previous
 * boot's abandoned command list happened to leave behind. */
extern uint64_t ui_canvas_hash[MU_CONTAINERPOOL_SIZE];
extern ui_canvas_marks_t ui_canvas_marks;

/* The transform in force, or identity while none set is valid; see
 * ui_set_transform() (ui_build.c). Both draw_command() and
 * canvas_physical_rect()/command_row_range() (ui.c) map through it. */
affine2x_t ui_effective_transform(void);

/* A point on the panel as the logical canvas sees it: what a raw input_t
 * touch needs before it means anything to a screen. */
void ui_to_logical(int x, int y, int* lx, int* ly);

/* The (font, scale) pair `font` was interned as by ui_set_font_scaled(), or
 * the shell default if `font` is NULL (asked for before ui_init() has run).
 * draw_command() and command_row_range() (ui.c) both need this to know what
 * a MU_COMMAND_TEXT command actually measures as. */
ui_font_scaled_t ui_resolve_font_scaled(mu_Font font);

#ifndef ESP_PLATFORM
/* Observes completed canvas paints without retaining device-side trace storage. */
extern void (*ui_canvas_painted_for_test)(const mu_Container*);
/* Observes face scope and identity misuse in host tests. */
extern void (*ui_face_misuse_for_test)(void);
/* Observes each face widget's label and rectangle in host tests. */
extern void (*ui_face_widget_for_test)(const char* label, recti_t r);
#endif

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
int ui_dropdown_label_width(int w, const ui_mu_theme_t* theme);

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

#define UI_FACE_IDS_MAX 32

void ui_face_init(void);
void ui_face_begin(const input_t* input);
void ui_face_end(void);

/* Closes the frame's command list: the open face window, microui's frame and
 * the pointer frame. Every ui_end*() starts with it; a test reading the list
 * without painting calls it alone. */
void ui_end_commands(void);
bool ui_icon_button_id(mu_Context* ctx, mu_Id id, mu_Rect r, const ui_widget_button_t* button, int label_len,
                       const ui_mu_theme_t* theme);
bool ui_tile_button_id(mu_Context* ctx, mu_Id id, mu_Rect r, const ui_widget_button_t* button, int label_len,
                       const ui_mu_theme_t* theme);
bool ui_slider_int_id(mu_Context* ctx, mu_Id id, int* value, int lo, int hi, int step, bool enabled);
bool ui_theme_slider_int_id(mu_Context* ctx, mu_Id id, mu_Rect r, int* value, int lo, int hi, int step, bool enabled,
                            const ui_mu_theme_t* theme);
int ui_dropdown_id(mu_Context* ctx, mu_Id mid, const char* id, mu_Rect r, const char* const* names,
                   const draw_image_t* const* icons, int count, int selected, bool enabled, const ui_mu_theme_t* theme);

/* The dropdown list open this frame, or NULL: at most one is. */
const mu_Container* ui_dropdown_open_list(void);

bool ui_toggle_id(mu_Context* ctx, mu_Id mid, mu_Rect r, const char* label, int label_len, bool* on, bool enabled,
                  const ui_mu_theme_t* theme);
