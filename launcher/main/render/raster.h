/*
 * raster: one r3d_lit_mesh_t drawn through a camera on both cores into a
 * scratch block the caller hands it, then upscaled into a destination
 * picture. On a host, or with core 1 busy, the second half runs inline.
 * Rendering at half the destination's size quarters the pixels and halves
 * the rows and spans.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "build_variant.h"
#include "render/camera.h"
#include "render/r3d_lit_mesh.h"

/* The caller's options; the scratch block holds everything else. */
typedef struct {
    const r3d_lit_mesh_t* mesh;
    int width, height; /* the size drawn at */
    uint16_t clear;    /* in the pixel format r3d_span.h describes */
    /* When not NULL, the picture raster_upscale() fills, destination_width
     * by destination_height. The colour target is then never cleared, since
     * upscaling puts the clear colour wherever nothing was drawn. */
    uint16_t* destination;
    int destination_width, destination_height;
    void* scratch; /* raster_scratch_bytes() of it */
} raster_t;

typedef struct {
    int clusters, triangles; /* what survived culling */
} raster_stats_t;

#if CONFIG_LAUNCHER_DEVELOPMENT
typedef struct {
    int64_t dispatch_us[2];
    int64_t core1_start_us[2];
    int64_t core0_work_us[2];
    int64_t core1_work_us[2];
    int64_t join_wait_us[2];
} raster_probe_t;

void raster_probe_enable(bool enabled);
const raster_probe_t* raster_probe(void);
#endif

/* Everything a raster works in (per-vertex, per-cluster, colour and depth)
 * as one block, from its mesh and size: the caller obtains it once, from
 * any memory, so none of it has to live in internal RAM. */
size_t raster_scratch_bytes(const raster_t* raster);

/* Draws the mesh as `camera` sees it, turned for the panel's `quarter`. */
raster_stats_t raster_draw(const raster_t* raster, const camera_t* camera, int quarter);

/* Fills `destination` from what raster_draw() last drew, both cores taking
 * half the rows. The destination is the same size (a copy) or twice it each
 * way, and then 4-byte aligned. */
void raster_upscale(const raster_t* raster);

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
