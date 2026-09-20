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

typedef struct {
    float x, y, z;
} rt_vec3_t;

/* An infinite plane: `point` is any point on it, `normal` is unit length. */
typedef struct {
    rt_vec3_t point;
    rt_vec3_t normal;
} rt_plane_t;

/* Hit distance for a ray against `plane`, ahead of `origin`. False for a
 * miss: parallel to the plane, or the crossing is behind the ray. */
bool rt_intersect_plane(rt_vec3_t origin, rt_vec3_t dir, rt_plane_t plane, float* t);

/* An oriented box: axis-aligned in its own frame, turned `sin_yaw`/`cos_yaw`
 * about the vertical (Y) axis in world space. */
typedef struct {
    rt_vec3_t center;
    rt_vec3_t half_extent;
    float sin_yaw, cos_yaw;
} rt_box_t;

/* Hit distance and outward world-space normal for a ray against `box`,
 * rotating the ray into the box's own frame and running a slab test there.
 * Reports the exit face rather than the entry when the ray starts inside.
 * False for a miss. */
bool rt_intersect_box(rt_vec3_t origin, rt_vec3_t dir, const rt_box_t* box, float* t_hit, rt_vec3_t* out_normal);

typedef struct {
    rt_vec3_t origin, forward, right, up;
    float half_fov_short_tan;
    int width, height;         /* the physical canvas rt_cornell_render_row() fills */
    int eff_width, eff_height; /* width/height swapped at an odd quarter - see .c */
    int quarter;
} rt_cornell_camera_t;

/* Builds the camera once per render. `width`/`height` are the physical
 * canvas (GFX_WIDTH/GFX_HEIGHT); `quarter` (0..3) is display_shell_quarter(),
 * read by the caller so this stays a pure function of its arguments. */
void rt_cornell_camera_init(rt_cornell_camera_t* cam, int width, int height, int quarter);

/* Traces row `y`'s `cam->width` pixels into `out_row`. */
void rt_cornell_render_row(const rt_cornell_camera_t* cam, int y, gfx_color_t* out_row);
