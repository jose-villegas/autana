/*
 * raster: r3d_instance_t meshes drawn through a view on both cores into a
 * scratch block the caller hands it, then upscaled into a destination
 * picture. One mesh is a count of one. Rendering at half the destination's
 * size quarters the pixels and halves the rows and spans.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gfx/target/gfx_target.h"
#include "render/r3d_instance.h"
#include "render/raster_attachment.h"
#include "render/raster_split.h"
#include "render/render_view.h"
#include "render/resolution/resolution.h"
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
    /* With upscaling, colour is not cleared: raster_upscale() supplies the
     * clear colour wherever depth says nothing was drawn. */
    bool upscaled;
    int destination_width, destination_height;
    camera_present_t present;
    upscale_t upscale;     /* maps retained in `scratch`, rebuilt when size changes */
    raster_split_t* split; /* optional feedback retained between continuous frames */
    void* scratch;         /* raster_scratch_bytes() of it */
    /* Drawn beside colour and depth, at most GFX_ATTACHMENTS_FURTHER_MAX. */
    const raster_attachment_t* const* attachments;
    int attachment_count;
};

/* The viewport a view frames: destination size when upscaled, otherwise drawn size. */
static inline viewport_t
raster_viewport(const raster_t* raster, int quarter) {
    return (viewport_t){raster->upscaled ? raster->destination_width : raster->width,
                        raster->upscaled ? raster->destination_height : raster->height, quarter};
}

typedef struct {
    int clusters, triangles; /* what survived culling */
} raster_stats_t;

/* The most vertices and clusters any mesh the raster draws has: what its
 * scratch block holds room for. */
int raster_vertex_capacity(const raster_t* raster);
int raster_cluster_capacity(const raster_t* raster);

/* The picture's attachments, colour and depth then `attachments`, over
 * every row, not yet carved. */
void raster_picture(const raster_t* raster, gfx_target_t* picture);

/* The picture's colour and depth from its first row, as raster_draw() left
 * them: its first two attachments. */
gfx_color_t* raster_color(const raster_t* raster);
uint16_t* raster_depth(const raster_t* raster);

/* The depth encoding's scale that picture was drawn with: a stored depth d
 * is the view depth raster_depth_near() R3D_DEPTH_NEAREST / d. Each draw
 * fits it to the nearest bound of what it kept, never under the view's
 * near_z, so far surfaces keep their depth steps (r3d_lens_set_depth_near()). */
float raster_depth_near(const raster_t* raster);

/* Everything a raster works in (per-vertex, per-cluster, the census list,
 * the picture's attachments and the upscale maps) as one block, from its
 * instances' meshes and size: the caller obtains it once, from any memory,
 * so none of it has to live in internal RAM. */
size_t raster_scratch_bytes(const raster_t* raster);

/* Draws every instance through the picture basis and fit in `view`. */
raster_stats_t raster_draw(const raster_t* raster, const render_view_t* view);

/* The census list lets a caller price sizes before drawing. It stays valid
 * in the same scratch block for unchanged view and instances.
 * Upscaled rasters may change render size with a fixed destination; other
 * rasters must keep the census size. */
size_t raster_culled_length(const raster_t* raster);
raster_stats_t raster_census(const raster_t* raster, const render_view_t* view);
void raster_draw_culled(const raster_t* raster, const render_view_t* view);

/* The half picture can hold the width and every drawn row, the row map
 * spreading an uneven height. */
/* Presentation reduces the chosen height; composition handles uneven row maps. */
resolution_step_t raster_present_size(resolution_step_t size, camera_present_t present);
bool raster_fits_half_picture(const raster_t* raster);
resolution_step_t raster_compose_size(const raster_t* raster);

/* Fills `destination` from what was last drawn, both cores taking
 * half the destination rows. The destination is at least the drawn size. */
void raster_upscale(raster_t* raster, uint16_t* destination, int width, int height);
/* raster_upscale() of `picture` (colour and depth at the drawn size; NULL is
 * what was last drawn) through raster->upscale.filter, marking `seams`
 * (GFX_SEAM_BYTES of the destination size) where it blends; NULL marks none. */
void raster_upscale_picture(raster_t* raster, const gfx_target_t* picture, uint16_t* destination, uint8_t* seams,
                            int width, int height);
