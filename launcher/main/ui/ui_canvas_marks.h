/*
 * ui_canvas_marks - which screen rect each window was last painted over, so
 * a window that stops being drawn has its rect repainted rather than left
 * showing what it last drew. Nothing redraws a rect on its own when a
 * window closes: the canvases still drawn are unchanged, and ui_end() only
 * repaints what changed or was drawn under.
 *
 * Pure bookkeeping keyed by container slot, no microui calls and no gfx, so
 * a host suite can check it (test/suites/suite_ui_canvas_marks.c).
 */
#pragma once

#include <stdbool.h>

#include "microui.h"

typedef struct {
    mu_Rect rect[MU_CONTAINERPOOL_SIZE]; /* physical */
    bool painted[MU_CONTAINERPOOL_SIZE];
} ui_canvas_marks_t;

void ui_canvas_marks_reset(ui_canvas_marks_t* marks);

void ui_canvas_marks_painted(ui_canvas_marks_t* marks, int slot, mu_Rect physical);

/* Every slot painted before but not `present` this frame: its rect into
 * `out`, and the slot forgotten. Returns how many were written. */
int ui_canvas_marks_take_vanished(ui_canvas_marks_t* marks, const bool present[MU_CONTAINERPOOL_SIZE], mu_Rect* out,
                                  int max);
