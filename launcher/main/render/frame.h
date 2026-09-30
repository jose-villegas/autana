/*
 * frame: one r3d_lit_mesh_t drawn through a camera on both cores, into
 * buffers the caller hands it, optionally doubled into a picture twice its
 * size. On a host, or with core 1 busy, the second half runs inline.
 * Rendering at half the size of the doubled picture and doubling the result
 * quarters the pixels and halves the rows and spans.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "render/camera.h"
#include "render/r3d_lit_mesh.h"

typedef struct r3d_pipeline_vertex r3d_pipeline_vertex_t;
typedef struct r3d_pipeline_rows r3d_pipeline_rows_t;

typedef struct {
    /* The caller's options; mesh, width and height before frame_use_scratch(). */
    const r3d_lit_mesh_t* mesh;
    int width, height;
    uint16_t clear; /* in the pixel format r3d_span.h describes */
    /* When not NULL, frame_double() writes the finished frame into this
     * picture, 2 * width by 2 * height, 4-byte aligned; the colour target is
     * then never cleared, since doubling puts the clear colour wherever
     * nothing was drawn. */
    uint16_t* doubled;

    /* Carved from the scratch block by frame_use_scratch(). */
    r3d_pipeline_vertex_t* cs; /* mesh->vertex_count entries */
    uint16_t* visible;         /* mesh->cluster_count entries */
    r3d_pipeline_rows_t* rows; /* mesh->cluster_count entries */
    uint16_t* color;           /* width * height */
    uint16_t* depth;           /* width * height */
} frame_t;

typedef struct {
    int clusters, triangles; /* what survived culling */
} frame_stats_t;

/* Everything a frame of `mesh` at width by height works in (per-vertex,
 * per-cluster, colour and depth) as one block: the caller obtains it once
 * and frame_use_scratch() carves it, so none of it has to live in
 * internal RAM. */
size_t frame_scratch_bytes(const r3d_lit_mesh_t* mesh, int width, int height);
void frame_use_scratch(frame_t* frame, void* scratch);

/* Draws the mesh as `camera` sees it, turned for the panel's `quarter`. */
frame_stats_t frame_draw(const frame_t* frame, const camera_t* camera, int quarter);

/* Doubles the frame frame_draw() last drew into `doubled`, both cores
 * taking half the rows. */
void frame_double(const frame_t* frame);

/* The 8x8 pixel tile FRAME_SHOW_DEPTH_TILES reduces the depth to: the unit
 * a hierarchical depth test would cull by. */
#define FRAME_SHOW_TILE 8

/* What frame_show() puts in the colour buffer. */
typedef enum {
    FRAME_SHOW_SHADED,     /* the baked colours as drawn */
    FRAME_SHOW_DEPTH,      /* the depth buffer as a grey ramp */
    FRAME_SHOW_DEPTH_TILES /* the farthest depth of each FRAME_SHOW_TILE square */
} frame_show_t;

/* Development builds only: a release caller fails at link. Between draw and
 * double, overwrites `frame->color` from `frame->depth`: nearest white,
 * farthest black over the drawn range; a tile is empty if any pixel is.
 * Empty pixels take `frame->clear`, as doubling does, so pick one that is
 * no grey. */
void frame_show(const frame_t* frame, frame_show_t mode);
