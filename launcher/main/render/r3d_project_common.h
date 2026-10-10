/* r3d_project_common: line near plane and pixel offset contract. */
#pragma once

/* A small fraction of one model unit: a caller with its own physical unit
 * (a meter, a grid cell) is free to pick a near_z of its own instead. */
#define R3D_LINE_NEAR_Z 0.1F

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
