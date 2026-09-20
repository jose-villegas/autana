/*
 * rt_cornell - a Whitted-style ray tracer for a Cornell box: one primary ray
 * per pixel, one point light with a hard shadow ray, no reflection or
 * refraction yet. Float throughout - see rt_cornell.c's own header for why.
 *
 * ESP-IDF-free and host-testable: nothing here reaches gfx beyond the pixel
 * type. `quarter` rolls the camera so the box stays upright under any of the
 * shell's four orientations, aspect included - see rt_cornell_camera_init().
 */
#pragma once

#include <stdbool.h>

#include "gfx/gfx_color.h"
#include "render/r3d_ray.h"

/* An infinite plane: `point` is any point on it, `normal` is unit length. */
typedef struct {
    r3d_vec3f_t point;
    r3d_vec3f_t normal;
} rt_plane_t;

/* Hit distance for a ray against `plane`, ahead of `origin`. False for a
 * miss: parallel to the plane, or the crossing is behind the ray. */
bool rt_intersect_plane(r3d_vec3f_t origin, r3d_vec3f_t dir, rt_plane_t plane, float* t);

/* An oriented box: axis-aligned in its own frame, turned `sin_yaw`/`cos_yaw`
 * about the vertical (Y) axis in world space. */
typedef struct {
    r3d_vec3f_t center;
    r3d_vec3f_t half_extent;
    float sin_yaw, cos_yaw;
} rt_box_t;

/* Hit distance and outward world-space normal for a ray against `box`,
 * rotating the ray into the box's own frame and running a slab test there.
 * Reports the exit face rather than the entry when the ray starts inside.
 * False for a miss. */
bool rt_intersect_box(r3d_vec3f_t origin, r3d_vec3f_t dir, const rt_box_t* box, float* t_hit, r3d_vec3f_t* out_normal);

typedef r3d_ray_camera_t rt_cornell_camera_t;

/* Builds the camera once per render. `width`/`height` are the physical
 * canvas (GFX_WIDTH/GFX_HEIGHT); `quarter` (0..3) is display_shell_quarter(),
 * read by the caller so this stays a pure function of its arguments. */
void rt_cornell_camera_init(rt_cornell_camera_t* cam, int width, int height, int quarter);

/* Traces row `y`'s `cam->viewport.width` pixels into `out_row`. */
/* One physical pixel, for a caller tracing in an order of its own. */
gfx_color_t rt_cornell_render_pixel(const rt_cornell_camera_t* cam, int x, int y);

void rt_cornell_render_row(const rt_cornell_camera_t* cam, int y, gfx_color_t* out_row);
