/* Private UI state and backend entry points shared by the frame builder,
 * face and painter. Screens use ui.h or ui_mu.h. */
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
#include "ui/ui_canvas_marks.h"

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

#include "ui/controls/ui_widgets.h"

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
