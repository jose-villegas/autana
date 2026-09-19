#pragma once

#include <stdint.h>

#include "app.h"

/* Which way is down in the screen plane, as input/tilt.h reports it: `gx`
 * and `gy` in the panel's own axes, `strength` 0-256 and `shake` 0-255. The
 * line eases toward level with it; never called, the line keeps boot's
 * landscape pose. */
void ui_ridge_set_gravity(int gx, int gy, int strength, int shake);

/* Puts the line at level now, with no easing and no hold after boot - for a
 * caller with no time to pass, such as a preview. */
void ui_ridge_settle(void);

/* Between ui_begin() and ui_end_over(): turns this frame's touch and shaking
 * into plucks, moves the line on, and redraws it if it moved or turned far
 * enough to see. Costs nothing while the line is at rest, level and
 * untouched. */
void ui_ridge_step(const input_t* input, uint32_t dt_ms);

/* The whole backdrop, black and the ridge as it stands - a ui_backdrop_fn. */
void ui_ridge_paint(void);
