/*
 * r3d_pipeline_count: what one frame's cull and draw did with a mesh,
 * counted beside them for development views and measurements; the frame's
 * own cull and draw count nothing. Development builds only, like every
 * render/view/ file.
 */
#pragma once

#include <stdint.h>

#include "render/r3d_lit_mesh.h"
#include "render/r3d_pipeline.h"

typedef struct {
    int nodes_visited, nodes_cut; /* nodes the walk reached; those whose box dropped their subtree */
    int clusters_tested, clusters_kept;
    int triangles_submitted;   /* every triangle of the kept clusters */
    int triangles_behind;      /* every corner behind the near plane */
    int triangles_clipped;     /* a corner behind the near plane or too far off screen: rebuilt and clipped */
    int triangles_missed;      /* no pixel centre inside: off the screen or between centres */
    int triangles_facing_away; /* single-sided, back-facing */
    int triangles_set_up;      /* handed to r3d_span whole */
} r3d_pipeline_counts_t;

/* The pending nodes of a counted walk and the frustum it tests them against. */
typedef struct {
    planef_t planes[PLANE_FRUSTUM_COUNT];
    uint16_t node[R3D_LIT_WALK_STACK_MAX];
    uint8_t mask[R3D_LIT_WALK_STACK_MAX];
} r3d_pipeline_count_walk_t;

/* The clusters r3d_pipeline_cull() keeps, in tree order rather than nearest
 * first, adding what the walk did to `counts`. `out` holds
 * mesh->cluster_count entries. */
int r3d_pipeline_cull_counted(const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens, uint16_t* out,
                              r3d_pipeline_count_walk_t* walk, r3d_pipeline_counts_t* counts);

/* Sorts each triangle of `clusters` as r3d_pipeline_draw() would into
 * `window`'s rows, or the whole screen when it is NULL, from `cs` as
 * r3d_pipeline_transform() left it, adding to `counts`. `set_up`, when not NULL, holds mesh->cluster_count entries and
 * each listed cluster's is how many of its triangles reach r3d_span, the
 * clipped ones included. */
void r3d_pipeline_count_triangles(const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens, const uint16_t* clusters,
                                  int count, const r3d_pipeline_vertex_t* cs, const r3d_span_target_t* window,
                                  r3d_pipeline_counts_t* counts, uint16_t* set_up);
