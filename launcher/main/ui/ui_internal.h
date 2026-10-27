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

enum { UI_COMMAND_IMAGE = MU_COMMAND_MAX, UI_COMMAND_SWATCHES };

typedef struct {
    mu_BaseCommand base;
    mu_Rect rect;
    mu_Color color;
    const image_t* image;
} ui_image_command_t;

typedef struct {
    mu_BaseCommand base;
    mu_Rect rect;
    int16_t cols, rows;
    uint32_t rgb[1];
} ui_swatch_command_t;

static inline mu_Rect
ui_swatch_cell_rect(const ui_swatch_command_t* grid, int col, int row) {
    const mu_Rect r = grid->rect;
    const int x0 = r.x + (col * r.w / grid->cols);
    const int x1 = r.x + ((col + 1) * r.w / grid->cols);
    const int y0 = r.y + (row * r.h / grid->rows);
    const int y1 = r.y + ((row + 1) * r.h / grid->rows);
    return (mu_Rect){x0, y0, x1 - x0, y1 - y0};
}

#if UINTPTR_MAX == UINT32_MAX
_Static_assert(sizeof(ui_image_command_t) == 32, "image command device budget");
#endif

/* Custom drawing commands share microui's icon clipping protocol. */
void* ui_push_clipped_command(mu_Context* ctx, int type, int size, mu_Rect rect);

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
vec2i_t ui_to_logical(vec2i_t point);

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

/* An image is one command; its runs are streamed at paint time. A NULL
 * image draws the centred text fallback in the current font. */
void ui_draw_icon(mu_Context* ctx, mu_Rect r, const image_t* icon, const char* text, mu_Color color);

/* An integer-valued slider over the next layout row, shaped like
 * mu_slider_ex(), but integer: that one's float/"%.2f" thumb is the wrong
 * shape for a "06 PX" control. Writes through `value`, returns whether it
 * changed this frame. */
bool ui_slider_int(mu_Context* ctx, int* value, int lo, int hi, int step);

#define UI_FACE_IDS_MAX 32

void ui_face_init(void);
void ui_face_begin(const input_t* input);
void ui_face_end(void);

/* Closes the frame's command list: the open face window, microui's frame and
 * the pointer frame. Every ui_end*() starts with it; a test reading the list
 * without painting calls it alone. */
void ui_end_commands(void);
bool ui_slider_int_id(mu_Context* ctx, mu_Id id, int* value, int lo, int hi, int step, bool enabled);

#include "ui/controls/ui_widgets.h"
