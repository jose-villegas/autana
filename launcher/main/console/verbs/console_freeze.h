/*
 * console_freeze (FREEZE, RESUME and STEP): holds the frame loop on the
 * frame the panel already shows, and lets it past one frame at a time.
 *
 * For reading what a single frame actually drew and sent, which a device
 * running at framerate never stands still long enough to show: freeze,
 * look, step, look again. Pairs with SCREENSHOT (console_screenshot.h),
 * which captures whatever the held frame left in gfx, and with the debug
 * overlays (gfx_set_debug_overlay(), gfx_set_leaf_overlay()), whose
 * borders mark one frame's sends and are gone by the next.
 *
 * The verbs only post a frame request (console_frame_request.h); the
 * frame loop applies it and asks whether to run each pass. See console.c's
 * top comment for why nothing here may draw on the console task.
 * Development builds only; see console.h.
 *
 * An app with update() (app.h) presents the frame it drew on the pass
 * that follows, so a held panel shows one frame behind what gfx last
 * drew, and the STEP that follows sends it.
 */
#pragma once

#include <stdbool.h>

#include "console/console_frame_request.h"

/* The frame loop, on every pass that takes requests, before any early
 * return: applies a FREEZE, RESUME or STEP among them. */
void console_freeze_apply(const console_frame_request_t* requests);

/* True to run this pass as usual, false to hold: no app step, nothing
 * sent. Once per pass: a true return spends one STEP's credit. */
bool console_freeze_frame_allowed(void);
