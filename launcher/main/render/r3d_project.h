/*
 * r3d_project - camera-space near-plane clip and perspective projection for
 * a caller that has already composed its own model*view matrix and wants a
 * screen pixel out the other end.
 *
 * Header-only, static inline, and ESP-IDF-free, so a host suite can check
 * every line of it without a panel - see test/suites/suite_r3d_project.c.
 * This owns none of a caller's units, timeline or resolution: `r3d_view_t`
 * carries the whole environment a camera-space point needs (matrix, focal
 * length, near clip, and where the projection plane lands on screen), so a
 * caller with its own scale and its own screen size passes it in rather
 * than this file assuming one.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifndef SMALL3DLIB_H
#define S3L_PIXEL_FUNCTION     r3d_unused_pixel
#define S3L_RESOLUTION_X       368 /* unused: no rasterizer runs here */
#define S3L_RESOLUTION_Y       448
#define S3L_Z_BUFFER           0 /* no rasterizer, no depth buffer to keep */
#define S3L_SORT               0 /* no rasterizer, nothing to sort */
#define S3L_MAX_TRIANGES_DRAWN 1 /* never called; small3dlib still sizes an internal array off this */
#include "small3dlib.h"

static inline void
r3d_unused_pixel(S3L_PixelInfo* pixel) {
    (void)pixel;
}
#endif

/* A small fraction of one S3L_F unit - a caller with its own physical unit
 * (a meter, a grid cell) is free to pick a near_z of its own instead. */
#define R3D_NEAR_Z (S3L_F / 10)

typedef struct {
    S3L_Mat4 matrix; /* model * view, composed by the caller */
    S3L_Unit focal;  /* 0 is orthographic, as small3dlib defines it */
    S3L_Unit near_z; /* camera-space clip plane, > 0 */
    int center_x;    /* screen pixel the optical axis lands on */
    int center_y;
    int scale; /* pixels per projection-plane unit S3L_F, both axes */
} r3d_view_t;

static inline S3L_Vec4
r3d_to_camera_space(S3L_Vec4 model_point, const r3d_view_t* view) {
    S3L_vec3Xmat4(&model_point, (S3L_Unit(*)[4])view->matrix);
    return model_point;
}

static inline void
r3d_camera_to_screen(S3L_Vec4 p, const r3d_view_t* view, int* screen_x, int* screen_y) {
    p.z = S3L_nonZero(p.z);
    S3L_perspectiveDivide(&p, view->focal);

    /* NOT S3L_mapProjectionPlaneToScreen(): its S3L_ScreenCoord defaults
     * to int16_t, and S3L_USE_WIDER_TYPES would widen S3L_Unit itself to
     * int64_t everywhere - a real cost with no native 64-bit ALU. This
     * repeats its formula but with just the multiply done in int64_t: a
     * near-camera point's already-divided p.x/p.y can be large enough to
     * overflow a 32-bit product here even though the final on/off-panel
     * result never does - gfx.c's clip_line() leans on the same trick. */
    *screen_x = (int)(view->center_x + ((int64_t)p.x * view->scale) / S3L_F);
    *screen_y = (int)(view->center_y - ((int64_t)p.y * view->scale) / S3L_F);
}

/* Draws if point is in front; checks visibility, avoids invalid coordinates. */
static inline bool
r3d_project_point_cs(S3L_Vec4 p, const r3d_view_t* view, int* screen_x, int* screen_y) {
    if (p.z <= view->near_z) {
        return false;
    }
    r3d_camera_to_screen(p, view, screen_x, screen_y);
    return true;
}

/* Clips to near plane; avoids screen wrap. Returns false if segment is at or
 * behind the plane. */
static inline bool
r3d_project_segment_cs(S3L_Vec4 p0, S3L_Vec4 p1, const r3d_view_t* view, int* ax, int* ay, int* bx, int* by) {
    const bool front0 = p0.z > view->near_z;
    const bool front1 = p1.z > view->near_z;

    if (!front0 && !front1) {
        return false;
    }

    if (front0 != front1) {
        /* Replace endpoint with crossing point using linear interpolation in
         * camera space. */
        S3L_Vec4* behind = front0 ? &p1 : &p0;
        const S3L_Vec4* front = front0 ? &p0 : &p1;
        const int64_t frac_q16 = ((int64_t)(view->near_z - behind->z) << 16) / (front->z - behind->z);

        behind->x += (int32_t)(((int64_t)(front->x - behind->x) * frac_q16) >> 16);
        behind->y += (int32_t)(((int64_t)(front->y - behind->y) * frac_q16) >> 16);
        behind->z = view->near_z;
    }

    r3d_camera_to_screen(p0, view, ax, ay);
    r3d_camera_to_screen(p1, view, bx, by);
    return true;
}
