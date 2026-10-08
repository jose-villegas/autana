/*
 * raster: r3d_instance_t meshes drawn through a camera on both cores into a
 * scratch block the caller hands it, then upscaled into a destination
 * picture. One mesh is a count of one. Rendering at half the destination's
 * size quarters the pixels and halves the rows and spans.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gfx/gfx_render_target.h"
#include "render/camera.h"
#include "render/r3d_instance.h"
#include "render/raster_attachment.h"
#include "render/upscale.h"

/* The caller's options; the scratch block holds everything else. */
struct raster {
    /* What is drawn: each instance in turn into the one picture, nearer ones
     * covering farther ones whichever is drawn first. One mesh is a count of
     * one. Scratch holds the largest mesh's working buffers and every instance's
     * census slot. */
    const r3d_instance_t* instances;
    int instance_count;
    int width, height; /* the size drawn at */
    uint16_t clear;    /* in the pixel format r3d_span.h describes */
    /* The camera frames by destination_width and destination_height. With
     * upscaling, colour is not cleared: raster_upscale() supplies the clear
     * colour wherever depth says nothing was drawn. */
    bool upscaled;
    int destination_width, destination_height;
    upscale_t upscale; /* maps retained in `scratch`, rebuilt when size changes */
    void* scratch;     /* raster_scratch_bytes() of it */
    /* Drawn beside colour and depth, at most GFX_ATTACHMENTS_MAX - GFX_ATTACHMENT_FURTHER. */
    const raster_attachment_t* const* attachments;
    int attachment_count;
};

typedef struct {
    int clusters, triangles; /* what survived culling */
} raster_stats_t;

/* The most vertices and clusters any mesh the raster draws has: what its
 * scratch block holds room for. */
int raster_vertex_capacity(const raster_t* raster);
int raster_cluster_capacity(const raster_t* raster);

/* The picture's attachments, colour and depth then `attachments`, over
 * every row, not yet carved. */
gfx_render_target_t raster_picture(const raster_t* raster);

/* The picture's colour and depth from its first row, as raster_draw() left
 * them: its first two attachments. */
gfx_color_t* raster_color(const raster_t* raster);
uint16_t* raster_depth(const raster_t* raster);

/* Everything a raster works in (per-vertex, per-cluster, the census list,
 * the picture's attachments and the upscale maps) as one block, from its
 * instances' meshes and size: the caller obtains it once, from any memory,
 * so none of it has to live in internal RAM. */
size_t raster_scratch_bytes(const raster_t* raster);

/* Draws every instance as `camera` sees it, turned for the panel's `quarter`. */
raster_stats_t raster_draw(const raster_t* raster, const camera_t* camera, int quarter);

/* The census list lets a caller price sizes before drawing. It stays valid
 * in the same scratch block for unchanged camera, quarter and instances.
 * Upscaled rasters may change render size with a fixed destination; other
 * rasters must keep the census size. */
size_t raster_culled_length(const raster_t* raster);
raster_stats_t raster_census(const raster_t* raster, const camera_t* camera, int quarter);
void raster_draw_culled(const raster_t* raster, const camera_t* camera, int quarter);

/* Fills `destination` from what was last drawn, both cores taking
 * half the destination rows. The destination is at least the drawn size. */
void raster_upscale(raster_t* raster, uint16_t* destination, int width, int height);

/* The tile a hierarchical depth test would cull by. */
#define RASTER_SHOW_TILE 8

raster_attachment_t raster_depth_view(void* state);
raster_attachment_t raster_depth_tiles_view(void* state);

/* Development builds only: paints attached views before upscale. */
void raster_show(const raster_t* raster);
void raster_show_map(const gfx_render_target_t* picture, int index, uint16_t clear,
                     gfx_color_t (*color_of)(const void* pixel, uint16_t clear));
