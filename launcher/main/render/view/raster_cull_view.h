/*
 * raster_cull_view: what culling did with each cluster this frame, as a
 * development view. A cluster is culled (the node walk dropped it), kept
 * with nothing set up (every triangle behind, off screen or facing away),
 * set up but hidden (it lost every depth test) or drawn (it owns pixels).
 *
 * Seen from the camera only drawn clusters show, so the view can draw a
 * second time from a spectator, above the scene or behind the camera, and
 * paint the camera's fates there, every cluster drawn. Its overlay draws one
 * tree level's node boxes and, from a spectator, the camera's frustum.
 *
 * Tunables: cullview.from (raster_cull_from_t), cullview.paint
 * (raster_cull_paint_t) and cullview.level
 * (the tree depth whose boxes are drawn, 0 none; the root's children are 1).
 */
#pragma once

#include "build/build_variant.h"

#include <stdint.h>

#include "gfx/gfx.h"
#include "render/r3d_pipeline.h"
#include "render/raster.h"
#include "render/view/r3d_pipeline_count.h"

typedef enum {
    RASTER_CULL_FROM_CAMERA,
    RASTER_CULL_FROM_TOP,   /* above the instances' bounds, looking down */
    RASTER_CULL_FROM_CHASE, /* behind and above the camera */
    RASTER_CULL_FROM_COUNT,
} raster_cull_from_t;

typedef enum {
    RASTER_CULL_PAINT_SHADED, /* the baked colours, culled clusters a sparse screen door with dashed borders, kept ones' solid in their fate */
    RASTER_CULL_PAINT_FATES, /* each cluster flat in its fate's colour */
    RASTER_CULL_PAINT_NODES, /* each cluster in its node's hue at cullview.level, kept by its fate */
    RASTER_CULL_PAINT_COUNT,
} raster_cull_paint_t;

typedef enum {
    RASTER_FATE_CULLED,
    RASTER_FATE_NOT_SET_UP,
    RASTER_FATE_HIDDEN,
    RASTER_FATE_DRAWN,
    RASTER_FATE_COUNT,
} raster_cluster_fate_t;

/* The frame's draws: the camera's, then from a spectator the culled
 * clusters alone and the kept ones alone, so culled geometry never hides the
 * kept, and the holes in its screen door show what is behind. */
typedef enum { RASTER_CULL_PASS_CAMERA, RASTER_CULL_PASS_CULLED, RASTER_CULL_PASS_KEPT } raster_cull_pass_t;

/* What the view holds room for, every instance together. */
#define RASTER_CULL_CLUSTERS_MAX  8192
#define RASTER_CULL_TRIANGLES_MAX 131072
/* The largest picture it layers: the panel's. */
#define RASTER_CULL_PIXELS_MAX    (GFX_WIDTH * GFX_HEIGHT)

/* One frame's counts, every instance summed, from the camera. */
typedef struct {
    r3d_pipeline_counts_t counts;
    int triangles_covering; /* own at least one pixel of the picture */
    int clusters[RASTER_FATE_COUNT];
} raster_cull_frame_t;

/* Each count by its name, X(name, member of raster_cull_frame_t): the one
 * order and spelling every listing of the counts uses. */
#define RASTER_CULL_FRAME_FIELDS(X)                                                                                    \
    X(nodes_visited, counts.nodes_visited)                                                                             \
    X(nodes_cut, counts.nodes_cut)                                                                                     \
    X(clusters_tested, counts.clusters_tested)                                                                         \
    X(clusters_kept, counts.clusters_kept)                                                                             \
    X(clusters_not_set_up, clusters[RASTER_FATE_NOT_SET_UP])                                                           \
    X(clusters_hidden, clusters[RASTER_FATE_HIDDEN])                                                                   \
    X(clusters_drawn, clusters[RASTER_FATE_DRAWN])                                                                     \
    X(triangles_submitted, counts.triangles_submitted)                                                                 \
    X(triangles_behind, counts.triangles_behind)                                                                       \
    X(triangles_clipped, counts.triangles_clipped)                                                                     \
    X(triangles_missed, counts.triangles_missed)                                                                       \
    X(triangles_facing_away, counts.triangles_facing_away)                                                             \
    X(triangles_set_up, counts.triangles_set_up)                                                                       \
    X(triangles_covering, triangles_covering)

typedef struct {
    raster_cull_from_t from;
    bool spectating; /* drawing from the spectator */
    int pass;        /* raster_cull_pass_t: which of the frame's draws is next */
    int clusters, triangles;
    int next_cluster, next_triangle; /* the next instance's first IDs, as the draw reaches it */
    const raster_t* raster;
    render_view_t camera, spectator;
    raster_cull_frame_t frame;
    uint8_t fate[RASTER_CULL_CLUSTERS_MAX];
    uint16_t set_up[RASTER_CULL_CLUSTERS_MAX];
    uint16_t node[RASTER_CULL_CLUSTERS_MAX]; /* each cluster's node at cullview.level */
    r3d_pipeline_count_walk_t walk;
    /* The tree walks' pending nodes, here rather than on the drawing task's stack. */
    uint16_t walk_node[R3D_LIT_WALK_STACK_MAX], walk_shown[R3D_LIT_WALK_STACK_MAX];
    uint8_t walk_depth[R3D_LIT_WALK_STACK_MAX];
    uint32_t covered[RASTER_CULL_TRIANGLES_MAX / 32];
    /* The culled clusters' picture, as painted, and its depth: R3D_DEPTH_EMPTY
     * where nothing of them shows. */
    gfx_color_t under_color[RASTER_CULL_PIXELS_MAX];
    uint16_t under_depth[RASTER_CULL_PIXELS_MAX];
} raster_cull_t;

raster_attachment_t raster_cull_view(void* state);

/* After the camera's draw: tallies its fates and, unless cullview.from is
 * the camera, writes the spectator's view to draw from next. */
bool raster_cull_second_view(void* state, const raster_t* raster, render_view_t* out, bool* listed);

/* Lines over the composed picture, `width` by `height` from (0, 0). */
void raster_cull_overlay(void* state, int width, int height);

/* The last frame's counts. */
raster_cull_frame_t raster_cull_frame(const raster_cull_t* state);

#if BUILD_VARIANT_DEVELOPMENT_OR_HOST
bool raster_cull_copy_view(void* destination, const void* source, const raster_t* raster);
#endif
