/*
 * r3d_project: camera-space near-plane clip and perspective projection for
 * a caller that has already composed its own model*view matrix and wants a
 * screen pixel out the other end.
 *
 * Header-only, static inline, and ESP-IDF-free, so a host suite can check
 * every line of it without a panel; see test/suites/suite_r3d_project.c.
 * This owns none of a caller's units, timeline or resolution: `r3d_line_view_t`
 * carries the whole environment a camera-space point needs (matrix, focal
 * length, near clip, and where the projection plane lands on screen), so a
 * caller with its own scale and its own screen size passes it in rather
 * than this file assuming one. Float, in the caller's own length unit.
 */
#pragma once

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "util/math/mat4f.h"
#include "util/math/vec3f.h"

/* A small fraction of one model unit: a caller with its own physical unit
 * (a meter, a grid cell) is free to pick a near_z of its own instead. */
#define R3D_LINE_NEAR_Z 0.1F

/* Past this a projected offset is off any panel; clamping first keeps the
 * float to int conversion defined for a point at the camera, whose divide
 * by a zero depth is an infinity or a NaN. */
#define R3D_PIXEL_LIMIT 1000000.0F

typedef struct {
    mat4f_t matrix; /* model * view, composed by the caller */
    float focal;    /* projection-plane distance; 0 is orthographic */
    float near_z;   /* camera-space clip plane, > 0 */
    int center_x;   /* screen pixel the optical axis lands on */
    int center_y;
    float scale; /* pixels per projection-plane unit, both axes */
} r3d_line_view_t;

static inline vec3f_t
r3d_to_camera_space(vec3f_t model_point, const r3d_line_view_t* view) {
    return mat4f_apply(&view->matrix, model_point);
}

/* 1 / z for z > 0, to about 1e-5 relative: a seed from the exponent's own
 * bits and two Newton steps, all multiplies; far below a pixel at any screen
 * offset. The S3's FPU has no divide, so
 * `/` is a libgcc routine of a hundred cycles or more, and a projected point
 * needs one. Garbage for z <= 0, which no caller projects. */
static inline float
r3d_reciprocal(float z) {
    uint32_t bits;
    memcpy(&bits, &z, sizeof bits);
    bits = 0x7EF311C7u - bits;
    float y;
    memcpy(&y, &bits, sizeof y);
    y = y * (2.0F - (z * y));
    return y * (2.0F - (z * y));
}

/* A pixel offset rounded to the nearest whole pixel, ties away from zero, so
 * it is symmetric about the centre and a reciprocal one ulp short of an exact
 * quotient still lands on its pixel.
 * Plain comparisons, not fminf/fmaxf: those are libm calls on this FPU, and
 * this runs four times per edge. A NaN fails both and lands on the limit. */
static inline int
r3d_pixel_offset(float offset) {
    if (!(offset < R3D_PIXEL_LIMIT)) {
        return (int)R3D_PIXEL_LIMIT;
    }
    if (!(offset > -R3D_PIXEL_LIMIT)) {
        return -(int)R3D_PIXEL_LIMIT;
    }
    return (int)(offset + (offset < 0.0F ? -0.5F : 0.5F));
}

static inline void
r3d_camera_to_screen(vec3f_t p, const r3d_line_view_t* view, int* screen_x, int* screen_y) {
    float gain = view->scale;
    if (view->focal != 0.0F) {
        gain *= view->focal * r3d_reciprocal(p.z);
    }
    *screen_x = view->center_x + r3d_pixel_offset(p.x * gain);
    *screen_y = view->center_y - r3d_pixel_offset(p.y * gain);
}

/* Draws if point is in front; checks visibility, avoids invalid coordinates. */
static inline bool
r3d_project_point_cs(vec3f_t p, const r3d_line_view_t* view, int* screen_x, int* screen_y) {
    if (p.z <= view->near_z) {
        return false;
    }
    r3d_camera_to_screen(p, view, screen_x, screen_y);
    return true;
}

/* Clips to near plane; avoids screen wrap. Returns false if segment is at or
 * behind the plane. */
static inline bool
r3d_project_segment_cs(vec3f_t p0, vec3f_t p1, const r3d_line_view_t* view, int* ax, int* ay, int* bx, int* by) {
    const bool front0 = p0.z > view->near_z;
    const bool front1 = p1.z > view->near_z;

    if (!front0 && !front1) {
        return false;
    }

    if (front0 != front1) {
        /* Replace endpoint with crossing point using linear interpolation in
         * camera space. */
        vec3f_t* behind = front0 ? &p1 : &p0;
        const vec3f_t* front = front0 ? &p0 : &p1;
        const float frac = (view->near_z - behind->z) / (front->z - behind->z);

        behind->x += (front->x - behind->x) * frac;
        behind->y += (front->y - behind->y) * frac;
        behind->z = view->near_z;
    }

    r3d_camera_to_screen(p0, view, ax, ay);
    r3d_camera_to_screen(p1, view, bx, by);
    return true;
}
