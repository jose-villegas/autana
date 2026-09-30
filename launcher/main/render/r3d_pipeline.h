/*
 * r3d_pipeline: the stages r3d_frame_draw() runs to draw a r3d_lit_mesh_t
 * through a camera: cluster culling, one transform per vertex, near-plane
 * clipping and r3d_span. Internal to render/: only it, its suites and host
 * tools include this.
 *
 * Split into stages so two cores can share a frame: r3d_pipeline_transform()
 * writes disjoint vertex ranges for disjoint cluster lists, and
 * r3d_pipeline_draw() only touches the rows of its own target. No
 * allocation and no file-scope state; every buffer is the caller's.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "render/r3d.h"
#include "render/r3d_lit_mesh.h"
#include "render/r3d_span.h"
#include "render/r3d_vec3f.h"

/* A camera made ready for one viewport and one mesh's position scale.
 * Lens space: x and y are already pixels at unit depth, turned for the
 * panel's quarter, so a screen position is centre + (x, y) / z. */
typedef struct {
    float m[3][4]; /* position ticks to lens space */
    float center_x, center_y;
    float near_z; /* model units */
    int width, height;
    float snap_cx, snap_cy; /* the centre in subpixels, plus R3D_SNAP_BIAS */
    float near_subpixels;   /* near_z / R3D_SUBPIXEL */
} r3d_lens_t;

/* 8 bytes: the screen position is snapped to r3d_span's subpixels once, so
 * every test on it is exact. A triangle with a vertex that has no position
 * here is rebuilt from the mesh, as near clipping needs. */
struct r3d_pipeline_vertex {
    int16_t sx, sy; /* 1/R3D_SUBPIXEL pixels, valid while iz > 0 */
    float iz;       /* near_z / z; 0 behind the near plane, below 0 in front but too far off screen to snap */
};

/* Turned for the viewport's quarter as r3d_ray.h maps it. */
void r3d_lens_init(r3d_lens_t* lens, const r3d_camera_t* camera, int position_scale, r3d_viewport_t viewport);

/* Walks the node tree and writes the clusters any part of which may be on
 * screen, roughly nearest first, returning how many. `out` holds
 * mesh->cluster_count entries. */
int r3d_pipeline_cull(const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens, uint16_t* out);

/* The screen rows a cluster's vertices span, for a caller drawing only some
 * rows to skip it whole. A cluster with a vertex behind the near plane or
 * too far off screen to snap is unbounded, and never skipped. */
struct r3d_pipeline_rows {
    float y0, y1;
    bool unbounded;
};

/* `rows`, when not NULL, holds mesh->cluster_count entries and is filled for
 * each cluster transformed. */
void r3d_pipeline_transform(const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens, const uint16_t* clusters, int count,
                            r3d_pipeline_vertex_t* cs, r3d_pipeline_rows_t* rows);

/* `rows` as r3d_pipeline_transform() filled it, or NULL to test every triangle. */
void r3d_pipeline_draw(const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens, const uint16_t* clusters, int count,
                       const r3d_pipeline_vertex_t* cs, const r3d_pipeline_rows_t* rows,
                       const r3d_span_target_t* target);
