/* ui_bridge: the pointer's events into microui, and what microui reports back. */
#pragma once

#include "input/input.h"
#include "microui.h"
#include "ui/input/ui_pointer.h"

/* Before mu_begin(): on a press frame seed the hover root and look for
 * scrollable content under the press, then step `p` with the logical `input`
 * and replay its events into `ctx`. */
void ui_bridge_feed(mu_Context* ctx, ui_pointer_t* p, const input_t* input);

/* After mu_end(): tell `p` whether the pointer rests on scrollable content,
 * and whether a press went under a stale hover root and took nothing. */
void ui_bridge_end(mu_Context* ctx, ui_pointer_t* p);
