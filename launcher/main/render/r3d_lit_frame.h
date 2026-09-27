/*
 * r3d_lit_frame - one whole frame of a r3d_lit_mesh_t on both cores: cull on the
 * caller's core, then each core transforms half the visible clusters and
 * clears and fills half the rows. On a host, or with core 1 busy, the
 * second half runs inline. Rendering at half the size of the doubled
 * picture and doubling the result quarters the pixels and halves the rows
 * and spans.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "render/r3d_lit_pipeline.h"

typedef struct {
    const r3d_lit_mesh_t* mesh;
    int width, height;
    r3d_lit_vertex_t* cs; /* mesh->vertex_count entries */
    uint16_t* visible;    /* mesh->cluster_count entries */
    r3d_lit_rows_t* rows; /* mesh->cluster_count entries */
    uint16_t* color;      /* width * height */
    uint16_t* depth;      /* width * height */
    uint16_t clear;       /* in the pixel format r3d_span.h describes */
    /* When not NULL, r3d_lit_frame_double() writes the finished frame into this
     * picture, 2 * width by 2 * height, 4-byte aligned; the colour target is
     * then never cleared, since doubling puts the clear colour wherever
     * nothing was drawn. */
    uint16_t* doubled;
} r3d_lit_frame_t;

typedef struct {
    int clusters, triangles; /* what survived culling */
} r3d_lit_stats_t;

/* Everything a frame of `mesh` at width by height works in - per-vertex,
 * per-cluster, colour and depth - as one block: the caller obtains it once
 * and r3d_lit_frame_use_scratch() carves it, after setting mesh, width and
 * height, so none of it has to live in internal RAM. */
size_t r3d_lit_frame_scratch_bytes(const r3d_lit_mesh_t* mesh, int width, int height);
void r3d_lit_frame_use_scratch(r3d_lit_frame_t* frame, void* scratch);

r3d_lit_stats_t r3d_lit_frame_render(const r3d_lit_frame_t* frame, const r3d_lit_view_t* view);

/* Doubles the frame r3d_lit_frame_render() last drew into `doubled`, both cores
 * taking half the rows. */
void r3d_lit_frame_double(const r3d_lit_frame_t* frame);
