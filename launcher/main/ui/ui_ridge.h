#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "gfx/gfx.h"
#include "input/input.h"

/* Which way is down in the screen plane, as input/tilt.h reports it: `gx`
 * and `gy` in the panel's own axes, `strength` 0-256 and `shake` 0-255. The
 * line eases toward level with it; never called, the line keeps boot's
 * landscape pose. */
void ui_ridge_set_gravity(int gx, int gy, int strength, int shake);

/* Breathing and the wave along the line, off unless turned on. With them the
 * launcher draws every frame; without, it is idle whenever untouched and
 * level. */
void ui_ridge_set_ambient(bool on);

/* Puts the line at level now, with no easing and no hold after boot - for a
 * caller with no time to pass, such as a preview. */
void ui_ridge_settle(void);

/* Between ui_begin() and ui_end_over(): turns this frame's touch and shaking
 * into plucks, moves the line on, and redraws it if it moved or turned far
 * enough to see. Costs nothing while the line is at rest, level and
 * untouched. */
void ui_ridge_step(const input_t* input, uint32_t dt_ms);

/* The whole layered backdrop and the ridge as it stands - a ui_backdrop_fn. */
void ui_ridge_paint(void);

#if CONFIG_LAUNCHER_SELFTEST || !defined(ESP_PLATFORM)
/* Starts a device perf arm from the settled landscape state. */
void ui_ridge_reset_for_test(void);
/* Puts the boot hold and the ambient ease-in back at their start. */
void ui_ridge_restart_boot_for_test(void);
/* A full paint of the ridge as it stands into `out`, one framebuffer's
 * worth, touching neither the ridge's bookkeeping nor gfx's dirty state. */
void ui_ridge_paint_reference_for_test(gfx_color_t* out);
/* Whether the switch between row and column strips is still dissolving in. */
bool ui_ridge_dissolving_for_test(void);
/* How many pose steps the fill's gradient trails the ridge by. */
int ui_ridge_gradient_lag_for_test(void);
#endif
