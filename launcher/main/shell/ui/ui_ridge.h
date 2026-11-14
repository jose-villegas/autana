/* ui_ridge: interactive ridge animation driven by touch, tilt and elapsed time. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "build/build_variant.h"
#include "gfx/gfx.h"
#include "gfx/target/gfx_color.h"
#include "input/input.h"
#include "math/geometry/recti.h"
#include "ui/ui.h"

#define RIDGE_CURVE_POINTS GFX_HEIGHT
#define RIDGE_CURVE_VIEW_H GFX_WIDTH
/* Columns repainted and dirtied together, 16 wide: one PSRAM cache line of
 * RGB565. */
#define RIDGE_COLUMN_GROUP 16

/* Read the curve from the engine pack and keep that pack mounted; only the first call reads. An unavailable or invalid curve releases the pack and draws a level ridge. */
void ui_ridge_load(void);

/* The loaded row, or NULL when the ridge is level. */
const int16_t* ui_ridge_curve(void);

/* Gravity in g, screen axes (x right, y down, z out), as input_gravity():
 * unit while held, zero in free fall. Shake is a unitless level, 0..1.
 * The line eases toward level; without calls it keeps boot's landscape pose. */
void ui_ridge_set_gravity(vec3f_t gravity, float shake);

/* Breathing and the wave along the line, off unless turned on. With them the
 * launcher draws every frame; without, it is idle whenever untouched and
 * level. */
void ui_ridge_set_ambient(bool on);

/* Puts the line at level now, with no easing and no hold after boot, for a
 * caller with no time to pass, such as a preview. */
void ui_ridge_settle(void);

/* Between ui_begin() and ui_end_over(): turns this frame's touch and shaking
 * into plucks, moves the line on, and redraws it if it moved or turned far
 * enough to see. Costs nothing while the line is at rest, level and
 * untouched. */
void ui_ridge_step(const input_t* input, uint32_t dt_ms);

/* The layered backdrop and the ridge as they stand, inside `area`: a
 * ui_backdrop_fn. The whole screen also catches the screen up with the ridge
 * (its gradient, a dissolve); a smaller area repaints what the step last
 * painted there, or, mid-dissolve, the whole screen, ending the dissolve. */
void ui_ridge_paint(recti_t area);

/* Where the last ui_ridge_step() painted, for ui_end_over(); NULL with no
 * ridge. */
const ui_fresh_t* ui_ridge_fresh(void);

#if BUILD_VARIANT_SELFTEST_OR_HOST
#include "physics/spring/spring_line.h"

/* The spring state owned by the ridge, or NULL before allocation. */
const spring_line_t* ui_ridge_spring_for_test(void);
/* Starts a device perf arm from the settled landscape state. */
void ui_ridge_reset_for_test(void);
/* Release state created by the test reset; pre-existing state and the curve remain shared. */
void ui_ridge_release_for_test(void);
/* Puts the boot hold and the ambient ease-in back at their start. */
void ui_ridge_restart_boot_for_test(void);
/* A full paint of the ridge as it stands into `out`, one framebuffer's
 * worth, touching neither the ridge's bookkeeping nor gfx's dirty state. */
void ui_ridge_paint_reference_for_test(gfx_color_t* out);
/* Whether the switch between row and column strips is still dissolving in. */
bool ui_ridge_dissolving_for_test(void);
/* Strip switches that arrived while a dissolve was still running. */
int ui_ridge_dissolve_restarts_for_test(void);
/* How many pose steps the fill's gradient trails the ridge by. */
int ui_ridge_gradient_lag_for_test(void);
/* How many times the gradient has turned, each repainting row by row. */
int ui_ridge_gradient_turns_for_test(void);
/* The spans [lo[i], hi[i]) merged until none overlap; how many are left. */
int ui_ridge_merge_spans_for_test(int16_t* lo, int16_t* hi, int count);
#endif
