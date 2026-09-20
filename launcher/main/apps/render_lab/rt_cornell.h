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

/* Builds the camera once per render. `width`/`height` are the physical
 * canvas (GFX_WIDTH/GFX_HEIGHT); `quarter` (0..3) is display_shell_quarter(),
 * read by the caller so this stays a pure function of its arguments. */
void rt_cornell_camera_init(rt_cornell_camera_t* cam, int width, int height, int quarter);

/* One physical pixel, for a caller tracing in an order of its own. */
gfx_color_t rt_cornell_render_pixel(const rt_cornell_camera_t* cam, int x, int y);

/* Traces row `y`'s `cam->viewport.width` pixels into `out_row`. */
void rt_cornell_render_row(const rt_cornell_camera_t* cam, int y, gfx_color_t* out_row);
