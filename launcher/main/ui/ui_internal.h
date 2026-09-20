/*
 * ui_internal - the seam between ui_build.c (builds a frame's command list;
 * host-portable) and ui.c (paints one; needs the real framebuffer). Neither
 * half links against gfx.c without the other, so nothing outside these two
 * files includes this - it is not a third public module, just the state and
 * helpers they share to stay one logical unit split across two link targets.
 */
#pragma once

#include <stdbool.h>

#include "gfx/gfx_font.h"
#include "microui.h"
#include "ui/ui_pointer.h"
#include "ui/ui_style.h"
#include "ui/ui_transform.h"

/* What a mu_Font actually points at - see ui_build.c's ui_set_font() comment
 * for why the scale rides inside the font rather than a separate
 * render-time setting. Shared because ui.c's draw_command() and
 * command_row_range() both resolve a command's font the same way. */
typedef struct {
    const gfx_font_t* font;
    int scale;
} ui_font_scaled_t;

/* The one shell-wide microui context - see ui.h's own top comment for why
 * there is exactly one. Defined in ui_build.c; ui.c paints whatever frame it
 * holds. */
extern mu_Context ctx;

/* Set by ui_invalidate()/ui_init() (ui_build.c), read and cleared by
 * ui_end()/ui_end_for_bands() (ui.c) - see ui.h's ui_invalidate() comment. */
extern bool invalidated;

/* The style MU_COMMAND_TEXT is drawn in - set by ui_set_text_style()
 * (ui_build.c), read by draw_command() (ui.c). */
extern ui_text_style_t text_style;

/* Touch-to-mouse state - fed by ui_begin() (ui_build.c). ui.c's ui_end()/
 * ui_end_for_bands() report back whether this frame's pointer sits over a
 * scrollable container, the one thing painting learns that building does not
 * already know. */
extern ui_pointer_t pointer;

/* Zeroed by ui_init() (ui_build.c) even though only ui.c's repaint-skip
 * (mark_changed_canvases()/repaint_marked_canvases()) ever reads or writes
 * it afterward - a canvas hash is meaningless until a frame has painted, but
 * "meaningless" has to start as a known value, not whatever the previous
 * boot's abandoned command list happened to leave behind. */
extern uint64_t canvas_hash[MU_CONTAINERPOOL_SIZE];

/* The transform in force, or identity while none set is valid - see
 * ui_set_transform() (ui_build.c). Both draw_command() and canvas_physical_
 * rect()/command_row_range() (ui.c) map through it. */
ui_transform_t ui_effective_transform(void);

/* A point on the panel as the logical canvas sees it - what a raw input_t
 * touch needs before it means anything to a screen. */
void ui_to_logical(int x, int y, int* lx, int* ly);

/* The (font, scale) pair `font` was interned as by ui_set_font_scaled(), or
 * the shell default if `font` is NULL (asked for before ui_init() has run).
 * draw_command() and command_row_range() (ui.c) both need this to know what
 * a MU_COMMAND_TEXT command actually measures as. */
ui_font_scaled_t ui_resolve_font_scaled(mu_Font font);
