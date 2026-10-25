/* ui_ridge: interactive ridge animation driven by touch, tilt and elapsed time. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "build/build_variant.h"
#include "gfx/gfx.h"
#include "gfx/target/gfx_color.h"
#include "input/input.h"

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

/* The whole layered backdrop and the ridge as it stands: a ui_backdrop_fn. */
void ui_ridge_paint(void);

#if BUILD_VARIANT_SELFTEST_OR_HOST
#include "math/motion/spring_line.h"

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
#endif
