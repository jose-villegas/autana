/*
 * lit_pipeline - draws a lit_mesh_t through a pinhole camera: cluster
 * culling, one transform per vertex, near-plane clipping and span_raster.
 *
 * Split into stages so two cores can share a frame: lit_transform() writes
 * disjoint vertex ranges for disjoint cluster lists, and lit_draw() only
 * touches the rows of its own target. No allocation and no file-scope
 * state; every buffer is the caller's.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lit_mesh.h"
#include "span_raster.h"

typedef struct {
    float x, y, z;
} lit_vec3_t;

/* Lens space: x and y are already pixels at unit depth, turned for the
 * panel's quarter, so a screen position is centre + (x, y) / z. */
typedef struct {
    float m[3][4]; /* position ticks to lens space */
    float center_x, center_y;
    float near_z; /* model units */
    int width, height;
} lit_view_t;

typedef struct {
    float x, y, z;    /* lens space */
    float sx, sy, iz; /* screen position and near_z / z, valid while z > near_z */
} lit_cs_vertex_t;

/* `forward` need not be normalised but must not be vertical. The lens is
 * fitted to the shorter axis of the upright picture; quarter numbers the
 * panel's turn as display_shell_quarter() does. */
void lit_view_look(lit_view_t* view, lit_vec3_t eye, lit_vec3_t forward, float half_fov_short_tan, float near_z,
                   int position_scale, int width, int height, int quarter);

/* Walks the node tree and writes the clusters any part of which may be on
 * screen, roughly nearest first, returning how many. `out` holds
 * mesh->cluster_count entries. */
int lit_cull_clusters(const lit_mesh_t* mesh, const lit_view_t* view, uint16_t* out);

/* The screen rows a cluster's vertices span, for a caller drawing only some
 * rows to skip it whole. A cluster reaching behind the near plane has no
 * bounded span and is never skipped. */
typedef struct {
    float y0, y1;
    bool crosses_near;
} lit_cluster_rows_t;

/* `rows`, when not NULL, holds mesh->cluster_count entries and is filled for
 * each cluster transformed. */
void lit_transform(const lit_mesh_t* mesh, const lit_view_t* view, const uint16_t* clusters, int count,
                   lit_cs_vertex_t* cs, lit_cluster_rows_t* rows);

/* `rows` as lit_transform() filled it, or NULL to test every triangle. */
void lit_draw(const lit_mesh_t* mesh, const lit_view_t* view, const uint16_t* clusters, int count,
              const lit_cs_vertex_t* cs, const lit_cluster_rows_t* rows, const span_target_t* target);
