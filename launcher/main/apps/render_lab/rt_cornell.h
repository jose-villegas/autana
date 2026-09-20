/*
 * rt_cornell - a Whitted-style ray tracer for a Cornell box: one primary ray
 * per pixel, one point light with a hard shadow ray, no reflection or
 * refraction yet. Float throughout - see rt_cornell.c's own header for why.
 * The tracer itself is rt_geometry.h; rt_cornell_scene.h is the box's data.
 *
 * ESP-IDF-free and host-testable: nothing here reaches gfx beyond the pixel
 * type. `quarter` rolls the camera so the box stays upright under any of the
 * shell's four orientations, aspect included - see rt_cornell_camera_init().
 */
#pragma once

#include "gfx/gfx_color.h"
#include "render/r3d_ray.h"

typedef r3d_ray_camera_t rt_cornell_camera_t;

/* Builds the camera once per render. `viewport` is the PHYSICAL canvas and
 * the shell's quarter, read by the caller so this stays a pure function of
 * its arguments. */
void rt_cornell_camera_init(rt_cornell_camera_t* cam, r3d_viewport_t viewport);

/* One physical pixel, for a caller tracing in an order of its own. */
gfx_color_t rt_cornell_render_pixel(const rt_cornell_camera_t* cam, int x, int y);

/* Traces row `y`'s `cam->viewport.width` pixels into `out_row`. */
void rt_cornell_render_row(const rt_cornell_camera_t* cam, int y, gfx_color_t* out_row);

/* Traces every new lattice pixel in rows [y0, y1) at `step` into `fb`
 * (`cam->viewport.width` x `cam->viewport.height`, row-major). Two calls
 * over a step-aligned split of [y0, y1) draw the same picture as one call
 * over the whole range - what lets the budgeted variant below split this
 * across two cores. */
void rt_cornell_render_rows(const rt_cornell_camera_t* cam, gfx_color_t* fb, int y0, int y1, int step);

/* Traces up to `pixel_budget` pixels' worth of lattice rows from `y0`,
 * split across this core and core 1 (util/job.h). Returns the next `y0`;
 * may overshoot height by less than one `step`, so a dirty rect built from
 * it clamps first. */
int rt_cornell_render_lattice_budget(const rt_cornell_camera_t* cam, gfx_color_t* fb, int y0, int step,
                                     int pixel_budget);

/* The ordered-dither quantize this scene resolves a linear colour through -
 * shared with rt_path.c so both scenes read the picture off one dithering
 * rule rather than two that could drift apart. `linear` channels above 1.0
 * clamp at the brightest level rather than wrapping. */
gfx_color_t rt_cornell_dither_quantize(r3d_vec3f_t linear, int x, int y);
