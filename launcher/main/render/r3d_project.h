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
 * than this file assuming one.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "util/mat4i.h"

/* A small fraction of one M4_ONE unit: a caller with its own physical unit
 * (a meter, a grid cell) is free to pick a near_z of its own instead. */
#define R3D_LINE_NEAR_Z (M4_ONE / 10)

typedef struct {
    m4_mat_t matrix;  /* model * view, composed by the caller */
    m4_unit_t focal;  /* 0 is orthographic */
    m4_unit_t near_z; /* camera-space clip plane, > 0 */
    int center_x;     /* screen pixel the optical axis lands on */
    int center_y;
    int scale; /* pixels per projection-plane unit M4_ONE, both axes */
} r3d_line_view_t;

static inline m4_vec4_t
r3d_to_camera_space(m4_vec4_t model_point, const r3d_line_view_t* view) {
    m4_vec3_transform(&model_point, (m4_unit_t(*)[4])view->matrix);
    return model_point;
}

static inline void
r3d_camera_to_screen(m4_vec4_t p, const r3d_line_view_t* view, int* screen_x, int* screen_y) {
    p.z = m4_non_zero(p.z);
    m4_perspective_divide(&p, view->focal);

    /* Only the multiply is 64-bit, not every unit: a near-camera point's
     * already-divided p.x/p.y can be large enough to overflow a 32-bit
     * product here even though the final on/off-panel result never does;
     * gfx.c's clip_line() leans on the same trick. */
    *screen_x = (int)(view->center_x + ((int64_t)p.x * view->scale) / M4_ONE);
    *screen_y = (int)(view->center_y - ((int64_t)p.y * view->scale) / M4_ONE);
}

/* Draws if point is in front; checks visibility, avoids invalid coordinates. */
static inline bool
r3d_project_point_cs(m4_vec4_t p, const r3d_line_view_t* view, int* screen_x, int* screen_y) {
    if (p.z <= view->near_z) {
        return false;
    }
    r3d_camera_to_screen(p, view, screen_x, screen_y);
    return true;
}

/* Clips to near plane; avoids screen wrap. Returns false if segment is at or
 * behind the plane. */
static inline bool
r3d_project_segment_cs(m4_vec4_t p0, m4_vec4_t p1, const r3d_line_view_t* view, int* ax, int* ay, int* bx, int* by) {
    const bool front0 = p0.z > view->near_z;
    const bool front1 = p1.z > view->near_z;

    if (!front0 && !front1) {
        return false;
    }

    if (front0 != front1) {
        /* Replace endpoint with crossing point using linear interpolation in
         * camera space. */
        m4_vec4_t* behind = front0 ? &p1 : &p0;
        const m4_vec4_t* front = front0 ? &p0 : &p1;
        const int64_t frac_q16 = ((int64_t)(view->near_z - behind->z) << 16) / (front->z - behind->z);

        behind->x += (int32_t)(((int64_t)(front->x - behind->x) * frac_q16) >> 16);
        behind->y += (int32_t)(((int64_t)(front->y - behind->y) * frac_q16) >> 16);
        behind->z = view->near_z;
    }

    r3d_camera_to_screen(p0, view, ax, ay);
    r3d_camera_to_screen(p1, view, bx, by);
    return true;
}
