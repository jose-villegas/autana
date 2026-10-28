/*
 * raster_cull_occlusion: how many of a frame's set-up clusters a whole-
 * cluster depth test before the draw could skip, under three conservative
 * bounds. Measured, not used: the cull view replays the camera's draw one
 * cluster at a time in each core's band, nearest first as the draw goes, and
 * tests each cluster against the depth its band holds so far:
 *
 * - rect: the box of its projected vertices and their nearest depth,
 *   scanned as r3d_span_hidden() scans a triangle's;
 * - corners: the same over its bounding box's 8 projected corners;
 * - tiles: the corners' box rounded out to RASTER_OCCLUSION_TILE square
 *   tiles, each tested by its farthest depth, kept current for free.
 *
 * A cluster is caught when its bound skips it in every band it sets up
 * triangles in; the triangles removed are counted per band. Development
 * builds only, behind cullview.occlusion; removable as its files plus the
 * cull view's call and field.
 */
#pragma once

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>

#include "render/r3d_pipeline.h"
#include "render/r3d_span.h"
#include "render/raster_internal.h"

typedef enum {
    RASTER_OCCLUSION_RECT,
    RASTER_OCCLUSION_CORNERS,
    RASTER_OCCLUSION_TILES,
    RASTER_OCCLUSION_BOUNDS,
} raster_occlusion_bound_t;

/* A tiles bound's tile, in pixels on a side. */
#define RASTER_OCCLUSION_TILE 8

/* One frame, every instance summed. */
typedef struct {
    int hidden;                                 /* clusters set up that own no pixel of the picture */
    int triangles_set_up;                       /* summed over the bands each reaches */
    int caught[RASTER_OCCLUSION_BOUNDS];        /* clusters skipped in every band they set up in */
    int hidden_caught[RASTER_OCCLUSION_BOUNDS]; /* of them, hidden ones */
    int removed[RASTER_OCCLUSION_BOUNDS];       /* triangles set up in the bands a cluster was skipped in */
    int64_t read[RASTER_OCCLUSION_BOUNDS];      /* pixels the test read; tiles for the tiles bound */
    int unsafe[RASTER_OCCLUSION_BOUNDS];        /* skipped in a band yet wrote depth there: must stay 0 */
} raster_occlusion_frame_t;

/* A frame's totals as one line, for a tool to read: each bound's counts
 * under its name, in raster_occlusion_bound_t's order. */
#define RASTER_OCCLUSION_LINE                                                                                          \
    "occlusion: hidden=%d triangles_set_up=%d"                                                                         \
    " rect_caught=%d rect_hidden_caught=%d rect_removed=%d rect_read=%" PRId64 " rect_unsafe=%d"                       \
    " corners_caught=%d corners_hidden_caught=%d corners_removed=%d corners_read=%" PRId64 " corners_unsafe=%d"        \
    " tiles_caught=%d tiles_hidden_caught=%d tiles_removed=%d tiles_read=%" PRId64 " tiles_unsafe=%d"

/* The replay's picture: `color` and `depth`, `width` by `height`, cleared
 * here, with the small-triangle fill the frame used. */
typedef struct {
    r3d_span_target_t picture;
    r3d_span_small_fill_t small_fill;
    int height;
    raster_occlusion_frame_t frame;
} raster_occlusion_t;

void raster_occlusion_begin(raster_occlusion_t* o, gfx_color_t* color, uint16_t* depth, int width, int height,
                            small_fill_t small_fill);

/* One instance through `lens`, drawn over the instances before it in the
 * order the frame drew it: nearest first when `culls`, else every cluster in
 * turn. `fate` holds its clusters' raster_cluster_fate_t. Uses `b`'s list,
 * rows, vertices and first work block. */
void raster_occlusion_instance(raster_occlusion_t* o, const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens,
                               const raster_buffers_t* b, bool culls, const uint8_t* fate);
