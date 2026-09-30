/*
 * r3d_lit_pipeline - draws a r3d_lit_mesh_t through a pinhole camera: cluster
 * culling, one transform per vertex, near-plane clipping and r3d_span.
 *
 * Split into stages so two cores can share a frame: r3d_lit_transform() writes
 * disjoint vertex ranges for disjoint cluster lists, and r3d_lit_draw() only
 * touches the rows of its own target. No allocation and no file-scope
 * state; every buffer is the caller's.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "render/r3d_camera.h"
#include "render/r3d_lit_mesh.h"
#include "render/r3d_span.h"
#include "render/r3d_vec3f.h"

/* Lens space: x and y are already pixels at unit depth, turned for the
 * panel's quarter, so a screen position is centre + (x, y) / z. */
typedef struct {
    float m[3][4]; /* position ticks to lens space */
    float center_x, center_y;
    float near_z; /* model units */
    int width, height;
    float snap_cx, snap_cy; /* the centre in subpixels, plus R3D_SNAP_BIAS */
    float near_subpixels;   /* near_z / R3D_SUBPIXEL */
} r3d_lit_view_t;

/* 8 bytes: the screen position is snapped to r3d_span's subpixels once, so
 * every test on it is exact. A triangle with a vertex that has no position
 * here is rebuilt from the mesh, as near clipping needs. */
typedef struct {
    int16_t sx, sy; /* 1/R3D_SUBPIXEL pixels, valid while iz > 0 */
    float iz;       /* near_z / z; 0 behind the near plane, below 0 in front but too far off screen to snap */
} r3d_lit_vertex_t;

/* `forward` need not be normalised but must not be vertical. The lens is
 * fitted to the shorter axis of the upright picture, turned for the
 * viewport's quarter as r3d_ray.h maps it. */
void r3d_lit_view_look(r3d_lit_view_t* view, r3d_vec3f_t eye, r3d_vec3f_t forward, float half_fov_short_tan,
                       float near_z, int position_scale, r3d_viewport_t viewport);

/* Walks the node tree and writes the clusters any part of which may be on
 * screen, roughly nearest first, returning how many. `out` holds
 * mesh->cluster_count entries. */
int r3d_lit_cull_clusters(const r3d_lit_mesh_t* mesh, const r3d_lit_view_t* view, uint16_t* out);

/* The screen rows a cluster's vertices span, for a caller drawing only some
 * rows to skip it whole. A cluster with a vertex behind the near plane or
 * too far off screen to snap is unbounded, and never skipped. */
typedef struct {
    float y0, y1;
    bool unbounded;
} r3d_lit_rows_t;

/* `rows`, when not NULL, holds mesh->cluster_count entries and is filled for
 * each cluster transformed. */
void r3d_lit_transform(const r3d_lit_mesh_t* mesh, const r3d_lit_view_t* view, const uint16_t* clusters, int count,
                       r3d_lit_vertex_t* cs, r3d_lit_rows_t* rows);

/* Measurement probe, never ships: a nonzero stop ends each triangle's draw
 * early, each stop including the ones before it; a triangle to be rebuilt
 * is skipped under all of them. */
#define R3D_LIT_PROBE 1 /* 0 compiles every stop out */

enum { R3D_LIT_PROBE_WALK = 1, R3D_LIT_PROBE_FETCH, R3D_LIT_PROBE_REJECT, R3D_LIT_PROBE_COLOR };

extern int r3d_lit_probe_stop;
extern volatile int32_t r3d_lit_probe_sink;

/* `rows` as r3d_lit_transform() filled it, or NULL to test every triangle. */
void r3d_lit_draw(const r3d_lit_mesh_t* mesh, const r3d_lit_view_t* view, const uint16_t* clusters, int count,
                  const r3d_lit_vertex_t* cs, const r3d_lit_rows_t* rows, const r3d_span_target_t* target);
