/*
 * console_freeze - FREEZE, RESUME and STEP: holds the frame loop on the
 * frame the panel already shows, and lets it past one frame at a time.
 *
 * For reading what a single frame actually drew and sent, which a device
 * running at framerate never stands still long enough to show: freeze,
 * look, step, look again. Pairs with SCREENSHOT (console_screenshot.h),
 * which captures whatever the held frame left in gfx, and with the debug
 * overlays (gfx_set_debug_overlay(), gfx_set_leaf_overlay()), whose
 * borders mark one frame's sends and are gone by the next.
 *
 * The verbs only latch; console_freeze_frame_allowed() is the frame
 * loop's side and the only thing that acts on them - see console.c's own
 * top comment for why nothing here may draw on the console task.
 * Development builds only - see console.h.
 *
 * An app with update() (app.h) presents the frame it drew on the pass
 * that follows, so a held panel shows one frame behind what gfx last
 * drew, and the STEP that follows sends it.
 */
#pragma once

#include <stdbool.h>

/* True when this pass should run the app and present as usual. False to
 * hold: the app does not step, nothing is sent, and the panel keeps what
 * it already shows.
 *
 * Call exactly once per pass, from the frame loop: a STEP's credit is
 * spent by the call that returns true for it, so a second caller would
 * eat a frame the first was given. */
bool console_freeze_frame_allowed(void);
