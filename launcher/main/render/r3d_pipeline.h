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

#include "math/linear/mat4f.h"
#include "math/scalar/mathi.h"
#include "render/camera.h"
#include "render/r3d_instance.h"
#include "render/r3d_lit_mesh.h"
#include "render/r3d_span.h"
#include "render/raster.h"
#include "render/viewport.h"

#if defined(ESP_PLATFORM)
#include "sdkconfig.h"
#endif

/* A camera made ready for one viewport and one mesh's position scale.
 * Lens space: x and y are already pixels at unit depth, turned for the
 * panel's quarter, so a screen position is centre + (x, y) / z. */
typedef struct {
    mat4f_t m; /* position ticks to lens space */
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

/* Refits a lens made for one picture size to a render `width` by `height`
 * stretched back to it on the way out, each axis on its own, so a render
 * need not keep the picture's shape. */
void r3d_lens_fit(r3d_lens_t* lens, int width, int height);

/* Carries the mesh into the world first: the lens matrix is composed with the
 * placement, so culling, transform and clipping see the mesh where it sits.
 * Call it after r3d_lens_init(), with the position scale of the mesh drawn. */
void r3d_lens_place(r3d_lens_t* lens, const r3d_placement_t* placement, int position_scale);

/* Caller-owned workspace, reusable between culling and drawing. Concurrent
 * draws each need their own block, aligned as malloc provides. */
typedef struct r3d_pipeline_work r3d_pipeline_work_t;
size_t r3d_pipeline_work_bytes(void);

/* Walks the node tree and writes the clusters any part of which may be on
 * screen, roughly nearest first, returning how many. `out` holds
 * mesh->cluster_count entries. */
int r3d_pipeline_cull(const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens, uint16_t* out, r3d_pipeline_work_t* work);

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

/* The length of the shortest prefix of `clusters` holding at least half of
 * their vertices; 0 when there are none. */
int r3d_pipeline_transform_split(const r3d_lit_mesh_t* mesh, const uint16_t* clusters, int count);

/* A row between 1 and height - 1 that divides a cheap estimate of raster
 * work: each visible cluster's triangle count across the buckets it spans. */
int r3d_pipeline_draw_split(const r3d_lit_mesh_t* mesh, const uint16_t* clusters, const r3d_pipeline_rows_t* rows,
                            int count, int height);

/* One way of the data cache: addresses this far apart share a cache set. */
#if defined(ESP_PLATFORM)
#define R3D_PIPELINE_CACHE_WAY (CONFIG_ESP32S3_DATA_CACHE_SIZE / CONFIG_ESP32S3_DCACHE_ASSOCIATED_WAYS)
#else
#define R3D_PIPELINE_CACHE_WAY 8192 /* the board's 64 KB in 8 ways */
#endif

/* Pointer-bearing state needs host alignment without widening the board's
 * four-byte carve. */
#define R3D_PIPELINE_WORK_ALIGNMENT _Alignof(r3d_span_target_t)

/* A raster's scratch block as its parts: the one layout
 * raster_scratch_bytes() sizes. The census list comes first, so it stays put
 * when the render size changes, and takes whole cache ways, so the parts
 * after it fall in the cache sets they would without it. */
typedef struct {
    r3d_pipeline_vertex_t* cs;    /* mesh->vertex_count entries */
    r3d_pipeline_rows_t* rows;    /* mesh->cluster_count entries */
    gfx_render_target_t picture;  /* raster_picture(), every row */
    r3d_pipeline_work_t* work[2]; /* one block per row slice */
    uint16_t* culled;             /* raster_culled_length() entries */
    void* draw;                   /* raster-owned instance and slice state */
} r3d_pipeline_buffers_t;

/* The census list's share of the scratch block: raster_culled_length()
 * entries, rounded up to whole cache ways. */
static inline size_t
r3d_pipeline_culled_bytes(const raster_t* raster) {
    const size_t way = R3D_PIPELINE_CACHE_WAY;
    return mathi_size_ceil(sizeof(uint16_t) * raster_culled_length(raster), way);
}

/* The lens `raster` draws through: framed on its picture's shape, fitted to
 * the size it renders at, for a mesh of `position_scale`. */
void raster_lens(const raster_t* raster, const camera_t* camera, int position_scale, int quarter, r3d_lens_t* lens);

static inline r3d_pipeline_buffers_t
r3d_pipeline_carve(const raster_t* raster) {
    char* p = raster->scratch;
    r3d_pipeline_buffers_t b;
    b.culled = (uint16_t*)p;
    p += r3d_pipeline_culled_bytes(raster);
    b.cs = (r3d_pipeline_vertex_t*)p;
    p += sizeof(r3d_pipeline_vertex_t) * (size_t)raster_vertex_capacity(raster);
    b.rows = (r3d_pipeline_rows_t*)p;
    p += sizeof(r3d_pipeline_rows_t) * (size_t)raster_cluster_capacity(raster);
    b.picture = raster_picture(raster);
    p = gfx_render_target_carve(&b.picture, p);
    p += gfx_attachment_bytes(sizeof(uint16_t), raster->destination_width + raster->destination_height, 1);
    p = (char*)raster->scratch + mathi_size_ceil((size_t)(p - (char*)raster->scratch), R3D_PIPELINE_WORK_ALIGNMENT);
    b.work[0] = (r3d_pipeline_work_t*)p;
    b.work[1] = (r3d_pipeline_work_t*)(p + r3d_pipeline_work_bytes());
    b.draw = p + 2 * r3d_pipeline_work_bytes();
    return b;
}
