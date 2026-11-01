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
#include "math/geometry/planef.h"
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
    float depth_near;       /* inverse-depth encoding scale */
} r3d_lens_t;

/* 8 bytes: the screen position is snapped to r3d_span's subpixels once, so
 * every test on it is exact. A triangle with a vertex that has no position
 * here is rebuilt from the mesh, as near clipping needs. */
typedef struct r3d_pipeline_vertex {
    int16_t sx, sy; /* 1/R3D_SUBPIXEL pixels, valid while iz > 0 */
    float iz;       /* depth_near / z; 0 behind the near plane, below 0 in front but too far off screen to snap */
} r3d_pipeline_vertex_t;

/* Scales the frame's ready basis to the mesh's position ticks. Depth is
 * encoded with depth_near = near_z until r3d_lens_set_depth_near(). */
void r3d_lens_init(r3d_lens_t* lens, const render_view_t* view, int position_scale);

/* The inverse-depth encoding's scale: a point at view depth z stores
 * depth_near / z, so depth_near is the nearest depth the 16 bits can tell
 * apart and the steps per unit at z are R3D_DEPTH_NEAREST depth_near / z^2.
 * Every instance of one picture must share it; the clip plane stays near_z. */
static inline void
r3d_lens_set_depth_near(r3d_lens_t* lens, float depth_near) {
    lens->depth_near = depth_near;
}

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

/* The planes of the lens's picture, in mesh ticks: near, then its four sides. */
static inline void
r3d_lens_frustum(const r3d_lens_t* lens, planef_t out[PLANE_FRUSTUM_COUNT]) {
    planef_frustum(&lens->m, &lens->project, rectf_from_edges(0.0F, 0.0F, (float)lens->width, (float)lens->height),
                   out);
}

/* The depth scale a picture's kept clusters allow, folded over its meshes:
 * depth_near starts at INFINITY (nothing kept) and only falls. */
typedef struct r3d_depth_fit {
    float near_z;     /* the clip plane, the least scale */
    float depth_near; /* inverse_depth_scale_fit(near_z, the nearest bound so far) */
} r3d_depth_fit_t;

/* Walks the node tree and writes the clusters any part of which may be on
 * screen, roughly nearest first, returning how many. `out` holds
 * mesh->cluster_count entries. A `fit` not NULL is lowered to the scale of a
 * bound on the view depth of every kept cluster's box where the frustum keeps
 * it (box3f_plane_distance_min_inside() per plane the box crosses); a box
 * reaching the near plane gives near_z. */
int r3d_pipeline_cull(const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens, uint16_t* out, r3d_pipeline_work_t* work,
                      r3d_depth_fit_t* fit);

/* The screen rows a cluster's vertices span, for a caller drawing only some
 * rows to skip it whole. A cluster with a vertex behind the near plane or
 * too far off screen to snap is unbounded, and never skipped. */
typedef struct {
    float y0, y1;
    bool unbounded;
} r3d_pipeline_rows_t;

/* `rows`, when not NULL, holds mesh->cluster_count entries and is filled for
 * each cluster transformed. */
void r3d_pipeline_transform(const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens, const uint16_t* clusters, int count,
                            r3d_pipeline_vertex_t* cs, r3d_pipeline_rows_t* rows);

/* `rows` as r3d_pipeline_transform() filled it, or NULL to test every triangle. */
void r3d_pipeline_draw(const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens, const uint16_t* clusters, int count,
                       const r3d_pipeline_vertex_t* cs, const r3d_pipeline_rows_t* rows,
                       const r3d_span_target_t* target, r3d_pipeline_work_t* work);

/* One mesh's visible clusters as the splits weigh them; several parts are
 * drawn as one, their clusters in part order. `rows` as transformed, or NULL
 * where only the transform split reads the part. */
typedef struct {
    const r3d_lit_mesh_t* mesh;
    const uint16_t* clusters;
    int count;
    const r3d_pipeline_rows_t* rows;
} r3d_pipeline_part_t;

/* The length of the shortest prefix of the parts' clusters, counted across
 * them in order, holding at least first_permille / JOB_SHARE_PERMILLE of their
 * vertices; 0 when there are none. */
int r3d_pipeline_transform_split(const r3d_pipeline_part_t* parts, int part_count, int first_permille);

/* A row between 1 and height - 1 assigning top_permille / JOB_SHARE_PERMILLE
 * of estimated draw weight to the top slice, interpolated within its bucket.
 * Weight is each visible cluster's triangle count across the buckets it spans.
 * With no weight, returns the middle row. */
int r3d_pipeline_draw_split(const r3d_pipeline_part_t* parts, int part_count, int height, int top_permille);

/* Pointer-bearing state needs host alignment without widening the board's
 * four-byte carve. */
#define R3D_PIPELINE_WORK_ALIGNMENT _Alignof(r3d_span_target_t)
