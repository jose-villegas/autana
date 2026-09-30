/*
 * r3d_lit_mesh - a triangle mesh with its lighting baked into one sRGB colour
 * per vertex, split into meshlet clusters. Each cluster owns a contiguous
 * range of vertices and triangles and indexes only its own vertices. The
 * clusters are the leaves of a tree rooted at nodes[0], so a subtree is
 * culled with one box test. `lod`, when present, adds coarser clusters
 * beside the tree; a renderer that ignores it draws the finest level.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint16_t vertex_first, vertex_count;
    uint16_t triangle_first, triangle_count;
    int16_t lo[3], hi[3]; /* bounds of its vertices, position ticks */
    bool double_sided;
} r3d_lit_cluster_t;

typedef struct {
    int16_t lo[3], hi[3]; /* bounds of everything below, position ticks */
    uint16_t first;       /* first child node, or a leaf's first cluster */
    uint8_t count;        /* children, or a leaf's clusters */
    bool leaf;
} r3d_lit_node_t;

/* A cluster's normal cone: axis and cutoff in 1/127ths. It culls a cluster
 * facing away; a cutoff of 127 means it never does. */
typedef struct {
    int8_t axis[3];
    int8_t cutoff;
} r3d_lit_cone_t;

/* A sphere and the error, in position ticks, of drawing coarser geometry
 * than the level a cluster was cut from. */
typedef struct {
    int16_t center[3];
    uint16_t radius;
    float error;
} r3d_lit_lod_bound_t;

/* Level 0 is the finest. A cluster is drawn when its own error, projected
 * at its sphere, is within the tolerance and its parent's is above it
 * (R3D_LIT_LOD_TOP: nothing coarser replaces it). docs/Mesh-Rendering.md has
 * the pick and the cone test. */
typedef struct {
    r3d_lit_lod_bound_t self, parent;
    r3d_lit_cone_t cone;
    uint8_t level;
} r3d_lit_cluster_lod_t;

#define R3D_LIT_LOD_TOP 3.0e38F

/* Every cluster above the finest, in vertex, colour and triangle arrays of
 * their own; `records` holds mesh->cluster_count finest entries, in cluster
 * order, then `cluster_count` of these. */
typedef struct {
    const int16_t (*positions)[3];
    const uint8_t (*colors)[3];
    const uint16_t (*triangles)[3];
    const r3d_lit_cluster_t* clusters;
    const r3d_lit_cluster_lod_t* records;
    int vertex_count, triangle_count, cluster_count;
    int level_count; /* including the finest */
} r3d_lit_lod_t;

typedef struct {
    const int16_t (*positions)[3]; /* ticks; position_scale ticks per model unit */
    const uint8_t (*colors)[3];
    const uint16_t (*triangles)[3]; /* counter-clockwise seen from the front */
    const r3d_lit_cluster_t* clusters;
    const r3d_lit_node_t* nodes; /* children of a node sit together */
    int vertex_count, triangle_count, cluster_count, node_count;
    int position_scale;
    const r3d_lit_cone_t* cones; /* one per cluster */
    const r3d_lit_lod_t* lod;    /* NULL when the mesh has no coarser levels */
} r3d_lit_mesh_t;
