/*
 * r3d_pipeline: the stages raster_draw() runs to draw a r3d_lit_mesh_t
 * through a camera: cluster culling, one transform per vertex, near-plane
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

#include "render/camera.h"
#include "render/r3d_lit_mesh.h"
#include "render/r3d_span.h"
#include "render/raster.h"
#include "render/vec3f.h"
#include "render/viewport.h"

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
typedef struct {
    int16_t sx, sy; /* 1/R3D_SUBPIXEL pixels, valid while iz > 0 */
    float iz;       /* near_z / z; 0 behind the near plane, below 0 in front but too far off screen to snap */
} r3d_pipeline_vertex_t;

/* Turned for the viewport's quarter as viewport.h maps it. */
void r3d_lens_init(r3d_lens_t* lens, const camera_t* camera, int position_scale, viewport_t viewport);

/* Walks the node tree and writes the clusters any part of which may be on
 * screen, roughly nearest first, returning how many. `out` holds
 * mesh->cluster_count entries. */
int r3d_pipeline_cull(const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens, uint16_t* out);

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
                       const r3d_span_target_t* target);

/* The prefix of visible clusters whose vertex work is closest to half. */
int r3d_pipeline_transform_split(const r3d_lit_mesh_t* mesh, const uint16_t* clusters, int count);

/* A row between 1 and height - 1 that divides a cheap estimate of raster
 * work: each visible cluster's triangle count across the buckets it spans. */
int r3d_pipeline_draw_split(const r3d_lit_mesh_t* mesh, const uint16_t* clusters, const r3d_pipeline_rows_t* rows,
                            int count, int height);

/* A raster's scratch block as its parts: the one layout
 * raster_scratch_bytes() sizes, widest alignment first so each part lands
 * aligned after the one before. */
typedef struct {
    r3d_pipeline_vertex_t* cs; /* mesh->vertex_count entries */
    r3d_pipeline_rows_t* rows; /* mesh->cluster_count entries */
    uint16_t* color;           /* width * height */
    uint16_t* depth;           /* width * height */
    uint16_t* visible;         /* mesh->cluster_count entries */
} r3d_pipeline_buffers_t;

static inline r3d_pipeline_buffers_t
r3d_pipeline_carve(const raster_t* raster) {
    const size_t pixels = (size_t)raster->width * (size_t)raster->height;
    char* p = raster->scratch;
    r3d_pipeline_buffers_t b;
    b.cs = (r3d_pipeline_vertex_t*)p;
    p += sizeof(r3d_pipeline_vertex_t) * (size_t)raster->mesh->vertex_count;
    b.rows = (r3d_pipeline_rows_t*)p;
    p += sizeof(r3d_pipeline_rows_t) * (size_t)raster->mesh->cluster_count;
    b.color = (uint16_t*)p;
    p += sizeof(uint16_t) * pixels;
    b.depth = (uint16_t*)p;
    p += sizeof(uint16_t) * pixels;
    b.visible = (uint16_t*)p;
    return b;
}
