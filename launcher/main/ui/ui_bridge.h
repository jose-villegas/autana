/* ui_bridge: the pointer's events into microui, and what microui reports back. */
#pragma once

#include "input/input.h"
#include "microui.h"
#include "ui/ui_pointer.h"

/* Before mu_begin(): step `p` with the logical `input`, replay its events
 * into `ctx`, and on a press frame seed the hover root. */
void ui_bridge_feed(mu_Context* ctx, ui_pointer_t* p, const input_t* input);

/* After mu_end(): tell `p` whether the pointer rests on scrollable content,
 * and whether the hover root it ran under was the one the frame found. */
void ui_bridge_end(mu_Context* ctx, ui_pointer_t* p);
