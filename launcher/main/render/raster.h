/*
 * raster: r3d_instance_t meshes drawn through a camera on both cores into a
 * scratch block the caller hands it, then upscaled into a destination
 * picture. One mesh is a count of one. Rendering at half the destination's
 * size quarters the pixels and halves the rows and spans.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "render/camera.h"
#include "render/r3d_instance.h"
#include "render/upscale.h"

/* The caller's options; the scratch block holds everything else. */
typedef struct {
    /* What is drawn: each instance in turn into the one picture, nearer ones
     * covering farther ones whichever is drawn first. One mesh is a count of
     * one. The scratch block holds room for the largest. */
    const r3d_instance_t* instances;
    int instance_count;
    int width, height; /* the size drawn at */
    uint16_t clear;    /* in the pixel format r3d_span.h describes */
    /* When not NULL, the picture raster_upscale() fills, destination_width
     * by destination_height. The colour target is then never cleared, since
     * upscaling puts the clear colour wherever nothing was drawn. */
    uint16_t* destination;
    int destination_width, destination_height;
    upscale_t upscale; /* maps retained in `scratch`, rebuilt when size changes */
    void* scratch;     /* raster_scratch_bytes() of it */
} raster_t;

typedef struct {
    int clusters, triangles; /* what survived culling */
} raster_stats_t;

/* The most vertices and clusters any mesh the raster draws has: what its
 * scratch block holds room for. */
int raster_vertex_capacity(const raster_t* raster);
int raster_cluster_capacity(const raster_t* raster);

/* Everything a raster works in (per-vertex, per-cluster, colour and depth)
 * as one block, from its instances' meshes and size: the caller obtains it once, from
 * any memory, so none of it has to live in internal RAM. */
size_t raster_scratch_bytes(const raster_t* raster);

/* Draws every instance as `camera` sees it, turned for the panel's `quarter`. */
raster_stats_t raster_draw(const raster_t* raster, const camera_t* camera, int quarter);

/* Fills `destination` from what raster_draw() last drew, both cores taking
 * half the destination rows. The destination is at least the drawn size. */
void raster_upscale(raster_t* raster);

/* The 8x8 pixel tile RASTER_SHOW_DEPTH_TILES reduces the depth to: the unit
 * a hierarchical depth test would cull by. */
#define RASTER_SHOW_TILE 8

/* What raster_show() puts in the colour buffer. */
typedef enum {
    RASTER_SHOW_SHADED,     /* the baked colours as drawn */
    RASTER_SHOW_DEPTH,      /* the depth buffer as a grey ramp */
    RASTER_SHOW_DEPTH_TILES /* the farthest depth of each RASTER_SHOW_TILE square */
} raster_show_t;

/* Development builds only: a release caller fails at link. Between draw and
 * upscale, overwrites the colour buffer from the depth: nearest white,
 * farthest black over the drawn range; a tile is empty if any pixel is.
 * Empty pixels take `clear`, as upscaling does, so pick one that is no
 * grey. */
void raster_show(const raster_t* raster, raster_show_t mode);
