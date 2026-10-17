/*
 * r3d_project: pixel snapping over math's projectf lens, the float reference for
 * the fixed-point line path.
 * Snaps to whole pixels only; no subpixel positions or guard band.
 * Header-only and ESP-IDF-free.
 */
#pragma once

#include <math.h>
#include <stdbool.h>

#include "math/linear/projectf.h"
#include "math/scalar/mathf.h"

/* Past this a projected offset is off any panel; clamping first keeps the
 * float to int conversion defined for a point at the camera, whose
 * reciprocal of a zero depth is an infinity or a NaN: a defined pixel, on
 * no particular side. */
#define R3D_PIXEL_LIMIT 1000000.0F

/* Above mathf_recip()'s error out to a 1000 pixel offset, far below a pixel. */
#define R3D_PIXEL_BIAS  0.02F

/* A pixel offset truncated toward zero, so it is symmetric about the centre.
 * A small bias first, so a reciprocal an ulp short of an exact quotient
 * still lands on its pixel.
 * A NaN fails both comparisons and lands on the limit. */
static inline int
r3d_pixel_offset(float offset) {
    if (!(offset < R3D_PIXEL_LIMIT)) {
        return (int)R3D_PIXEL_LIMIT;
    }
    if (!(offset > -R3D_PIXEL_LIMIT)) {
        return -(int)R3D_PIXEL_LIMIT;
    }
    return (int)(offset + (offset < 0.0F ? -R3D_PIXEL_BIAS : R3D_PIXEL_BIAS));
}

static inline void
r3d_project_screen(vec3f_t p, const projectf_t* lens, int* screen_x, int* screen_y) {
    const vec2f_t offset = projectf_offset(lens, p);
    *screen_x = (int)lens->cx + r3d_pixel_offset(offset.x);
    *screen_y = (int)lens->cy - r3d_pixel_offset(offset.y);
}

/* False when the point is at or behind the near plane. */
static inline bool
r3d_project_point_cs(vec3f_t p, const projectf_t* lens, int* screen_x, int* screen_y) {
    if (p.z <= lens->near_z) {
        return false;
    }
    r3d_project_screen(p, lens, screen_x, screen_y);
    return true;
}

/* Clips to near plane; avoids screen wrap. Returns false if segment is at or
 * behind the plane. */
static inline bool
r3d_project_segment_cs(vec3f_t p0, vec3f_t p1, const projectf_t* lens, int* ax, int* ay, int* bx, int* by) {
    if (!projectf_clip_near(lens->near_z, &p0, &p1)) {
        return false;
    }

    r3d_project_screen(p0, lens, ax, ay);
    r3d_project_screen(p1, lens, bx, by);
    return true;
}
