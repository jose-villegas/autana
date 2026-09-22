#pragma once

#include <stdint.h>

#include "input/input.h"
#include "microui.h"

struct app;

void ui_launcher_init(void);

/* The home screen's own rows, with no ui_begin()/ui_end() around them - see
 * this module's own top comment for why, and docs/Building-a-Screen.md for
 * the same split every app's own screen already keeps. Returns the app the
 * user picked, or NULL if none. */
const struct app* ui_launcher_draw(mu_Context* ctx, uint32_t dt_ms);

/* ui_begin(), ui_launcher_draw(), ui_end() - what main.c calls once per
 * frame. `dt_ms` drives the scroll view's own momentum, unused while this
 * screen keeps the default (none). */
const struct app* ui_launcher_frame(const input_t* input, uint32_t dt_ms);
