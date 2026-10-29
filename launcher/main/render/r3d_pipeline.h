/*
 * r3d_pipeline: the stages raster_draw() runs to draw a r3d_lit_mesh_t
 * through a view: cluster culling, one transform per vertex, near-plane
 * clipping and r3d_span. Internal: render/ and any suite or host tool
 * include it.
 *
 * Split into stages so two cores can share a frame: r3d_pipeline_transform()
 * writes disjoint vertex ranges for disjoint cluster lists, and
 * r3d_pipeline_draw() only touches the rows of its own target. No
 * allocation and no file-scope state; every buffer is the caller's.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "display/viewport.h"
#include "math/linear/mat4f.h"
#include "math/projection/projectf.h"
#include "math/scalar/mathi.h"
#include "render/r3d_instance.h"
#include "render/r3d_lit_mesh.h"
#include "render/r3d_span.h"
#include "render/raster_split.h"
#include "render/render_view.h"

/* A view made ready for one viewport and one mesh's position scale. */
typedef struct r3d_lens {
    mat4f_t m;          /* position ticks to lens space */
    projectf_t project; /* m already applies the focal lengths and turn, so lens space's pinhole is {1, -1}. */
    int width, height;
    float snap_cx, snap_cy; /* the centre in subpixels, plus R3D_SNAP_BIAS */
    float near_subpixels;   /* near_z / R3D_SUBPIXEL */
} r3d_lens_t;

/* 8 bytes: the screen position is snapped to r3d_span's subpixels once, so
 * every test on it is exact. A triangle with a vertex that has no position
 * here is rebuilt from the mesh, as near clipping needs. */
typedef struct r3d_pipeline_vertex {
    int16_t sx, sy; /* 1/R3D_SUBPIXEL pixels, valid while iz > 0 */
    float iz;       /* near_z / z; 0 behind the near plane, below 0 in front but too far off screen to snap */
} r3d_pipeline_vertex_t;

/* Scales the frame's ready basis to the mesh's position ticks. */
void r3d_lens_init(r3d_lens_t* lens, const render_view_t* view, int position_scale);

/* Refits a lens made for one picture size to a render `width` by `height`
 * stretched back to it on the way out, each axis on its own, so a render
 * need not keep the picture's shape. */
void r3d_lens_fit(r3d_lens_t* lens, int width, int height);

/* Carries the mesh into the world first: the lens matrix is composed with the
 * model, so culling, transform and clipping see the mesh where it sits.
 * Call it after r3d_lens_init(), with the position scale of the mesh drawn. */
void r3d_lens_place(r3d_lens_t* lens, const mat4f_t* model, int position_scale);

/* Caller-owned workspace, reusable between culling and drawing. Concurrent
 * draws each need their own block, aligned as malloc provides. */
typedef struct r3d_pipeline_work r3d_pipeline_work_t;
size_t r3d_pipeline_work_bytes(void);

/* Walks the node tree and writes the clusters any part of which may be on
 * screen, roughly nearest first, returning how many. `out` holds
 * mesh->cluster_count entries. */
int r3d_pipeline_cull(const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens, uint16_t* out, r3d_pipeline_work_t* work);

/* The screen box a cluster's vertices span and the nearest depth its fill
 * can write, for a caller drawing only some rows, or over depth already
 * drawn, to skip it whole. A cluster with a vertex behind the near plane or
 * too far off screen to snap is unbounded, and never skipped. */
typedef struct {
    int16_t x0, y0, x1, y1; /* 1/R3D_SUBPIXEL pixels */
    uint16_t depth;         /* no depth its fill writes is nearer */
    bool unbounded;
} r3d_pipeline_rows_t;

/* True when every pixel of `target` under a cluster's box already holds
 * depth nearer than any its fill writes, so drawing it would change nothing,
 * attachments included. False for an unbounded cluster. */
bool r3d_pipeline_cluster_hidden(const r3d_pipeline_rows_t* rows, const r3d_span_target_t* target);

/* `rows`, when not NULL, holds mesh->cluster_count entries and is filled for
 * each cluster transformed. */
void r3d_pipeline_transform(const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens, const uint16_t* clusters, int count,
                            r3d_pipeline_vertex_t* cs, r3d_pipeline_rows_t* rows);

/* `rows` as r3d_pipeline_transform() filled it, or NULL to test every triangle. */
void r3d_pipeline_draw(const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens, const uint16_t* clusters, int count,
                       const r3d_pipeline_vertex_t* cs, const r3d_pipeline_rows_t* rows,
                       const r3d_span_target_t* target, r3d_pipeline_work_t* work);

/* The length of the shortest prefix of `clusters` holding at least
 * first_permille / JOB_SHARE_PERMILLE of their vertices; 0 when there are none. */
int r3d_pipeline_transform_split(const r3d_lit_mesh_t* mesh, const uint16_t* clusters, int count, int first_permille);

/* A row between 1 and height - 1 assigning top_permille / JOB_SHARE_PERMILLE
 * of estimated draw weight to the top slice, interpolated within its bucket.
 * Weight is each visible cluster's triangle count across the buckets it spans.
 * With no weight, returns the middle row. */
int r3d_pipeline_draw_split(const r3d_lit_mesh_t* mesh, const uint16_t* clusters, const r3d_pipeline_rows_t* rows,
                            int count, int height, int top_permille);

/* Pointer-bearing state needs host alignment without widening the board's
 * four-byte carve. */
#define R3D_PIPELINE_WORK_ALIGNMENT _Alignof(r3d_span_target_t)
