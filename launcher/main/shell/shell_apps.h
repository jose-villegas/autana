/*
 * shell_apps: switching between the launcher and the running app, and
 * stepping whichever is showing. `current` is NULL while a system screen,
 * the launcher or Control Center, shows.
 */
#pragma once

#include <stdint.h>

#include "app/app.h"
#include "input/gesture.h"
#include "input/input.h"

/* Once, before the first pass: the launcher showing, its backdrop level. */
void shell_apps_init(void);

/* The physical edge the content's logical bottom lies on at `quarter`, where
 * the home swipe starts; the opposite edge opens Control Center. */
gesture_edge_t shell_exit_edge_for_quarter(int quarter);

/* `next` becomes the running app: its enter() now, its first frame() on the
 * next pass, after a full redraw. */
void shell_start_app(const app_t** current, const app_t* next);

/* The app's exit(), then the systems' app_exit phase and the arena emptied;
 * `*current` becomes NULL. */
void shell_exit_app(const app_t** current);

/* shell_exit_app(), then the launcher drawn in the same pass, so the frame
 * presented next is the home screen rather than the app's last one. */
void shell_leave_app(const app_t** current, input_t* input, gesture_edge_t exit_edge, uint32_t dt_ms);

/* One pass of whichever is showing. An app that asked to leave, or was sent
 * home, leaves before its frame() sees this pass's input. */
void shell_step_app(const app_t** current, input_t* input, uint32_t dt_ms);

/* Presents this pass's frame, unless the running app's present is deferred
 * to overlap its next update(). */
void shell_present_unless_deferred(const app_t* current);
