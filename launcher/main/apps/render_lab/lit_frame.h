/*
 * lit_frame - one whole frame of a lit_mesh_t on both cores: cull on the
 * caller's core, then each core transforms half the visible clusters and
 * clears and fills half the rows. On a host, or with core 1 busy, the
 * second half runs inline. Rendering at half the panel's resolution and
 * doubling the result quarters the pixels and halves the rows and spans.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "gfx/gfx_color.h"
#include "lit_pipeline.h"

/* Taller frames split at the middle row instead of balancing the cores. */
#define LIT_FRAME_MAX_HEIGHT 448

typedef struct {
    const lit_mesh_t* mesh;
    lit_cs_vertex_t* cs;      /* mesh->vertex_count entries */
    uint16_t* visible;        /* mesh->cluster_count entries */
    lit_cluster_rows_t* rows; /* mesh->cluster_count entries */
    gfx_color_t* color;       /* width * height */
    uint16_t* depth;          /* width * height */
    int width, height;
    gfx_color_t clear;
    /* When not NULL, each core also doubles its finished rows into this
     * picture, 2 * width by 2 * height, 4-byte aligned. */
    gfx_color_t* doubled;
} lit_frame_t;

typedef struct {
    int clusters, triangles; /* what survived culling */
} lit_frame_stats_t;

/* The per-vertex and per-cluster working state a frame of `mesh` needs, as
 * one block: the caller obtains it once and lit_frame_use_scratch() carves
 * it, so none of it has to live in internal RAM. */
size_t lit_frame_scratch_bytes(const lit_mesh_t* mesh);
void lit_frame_use_scratch(lit_frame_t* frame, void* scratch);

lit_frame_stats_t lit_frame_render(const lit_frame_t* frame, const lit_view_t* view);
