/* The render context's frame clock, for render_context.c and its tests. */
#pragma once
#include <stdint.h>

#include "render/context/render_context.h"

/* The frame before, shown from its draw's start until `now_us`, teaches the
 * scaled budget when a policy drew and timed it; `waited_us` is
 * gfx_present_waited_us() now. render_context_draw() calls it with the clocks
 * before it draws. */
void render_context_shown(render_context_t* c, int64_t now_us, int64_t waited_us);
