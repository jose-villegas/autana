/*
 * r3d: what a scene includes to draw a baked lit mesh or trace rays. A
 * camera is placed in model units; a frame draws one mesh through it on
 * both cores, into buffers the caller hands it, optionally doubled into a
 * picture twice its size. Cull, transform and fill (r3d_pipeline.h,
 * r3d_span.h) are render/'s own, public only to its suites and host tools.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "render/r3d_lit_mesh.h"
#include "render/r3d_ray.h"
#include "render/r3d_vec3f.h"
#include "render/r3d_viewport.h"

/* A pinhole camera. Its lens is fitted to the shorter axis of the upright
 * picture; `forward` need not be normalised but must not be vertical. */
typedef struct {
    r3d_vec3f_t eye, forward; /* model units */
    float half_fov_short_tan;
    float near_z; /* model units */
} r3d_camera_t;

typedef struct r3d_pipeline_vertex r3d_pipeline_vertex_t;
typedef struct r3d_pipeline_rows r3d_pipeline_rows_t;

typedef struct {
    /* The caller's options; mesh, width and height before r3d_frame_use_scratch(). */
    const r3d_lit_mesh_t* mesh;
    int width, height;
    uint16_t clear; /* in the pixel format r3d_span.h describes */
    /* When not NULL, r3d_frame_double() writes the finished frame into this
     * picture, 2 * width by 2 * height, 4-byte aligned; the colour target is
     * then never cleared, since doubling puts the clear colour wherever
     * nothing was drawn. */
    uint16_t* doubled;

    /* Carved from the scratch block by r3d_frame_use_scratch(). */
    r3d_pipeline_vertex_t* cs; /* mesh->vertex_count entries */
    uint16_t* visible;         /* mesh->cluster_count entries */
    r3d_pipeline_rows_t* rows; /* mesh->cluster_count entries */
    uint16_t* color;           /* width * height */
    uint16_t* depth;           /* width * height */
} r3d_frame_t;

typedef struct {
    int clusters, triangles; /* what survived culling */
} r3d_frame_stats_t;

/* Everything a frame of `mesh` at width by height works in (per-vertex,
 * per-cluster, colour and depth) as one block: the caller obtains it once
 * and r3d_frame_use_scratch() carves it, so none of it has to live in
 * internal RAM. */
size_t r3d_frame_scratch_bytes(const r3d_lit_mesh_t* mesh, int width, int height);
void r3d_frame_use_scratch(r3d_frame_t* frame, void* scratch);

/* Draws the mesh as `camera` sees it, turned for the panel's `quarter`. */
r3d_frame_stats_t r3d_frame_draw(const r3d_frame_t* frame, const r3d_camera_t* camera, int quarter);

/* Doubles the frame r3d_frame_draw() last drew into `doubled`, both cores
 * taking half the rows. */
void r3d_frame_double(const r3d_frame_t* frame);

/* The 8x8 pixel tile R3D_SHOW_DEPTH_TILES reduces the depth to: the unit
 * a hierarchical depth test would cull by. */
#define R3D_SHOW_TILE 8

/* What r3d_frame_show() puts in the colour buffer. */
typedef enum {
    R3D_SHOW_SHADED,     /* the baked colours as drawn */
    R3D_SHOW_DEPTH,      /* the depth buffer as a grey ramp */
    R3D_SHOW_DEPTH_TILES /* the farthest depth of each R3D_SHOW_TILE square */
} r3d_show_t;

/* Development builds only: a release caller fails at link. Between draw and
 * double, overwrites `frame->color` from `frame->depth`: nearest white,
 * farthest black over the drawn range; a tile is empty if any pixel is.
 * Empty pixels take `frame->clear`, as doubling does, so pick one that is
 * no grey. */
void r3d_frame_show(const r3d_frame_t* frame, r3d_show_t mode);
