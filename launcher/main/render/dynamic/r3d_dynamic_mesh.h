/*
 * r3d_dynamic_mesh: a lit mesh rebuilt from caller triangles, for geometry
 * made at run time (a fluid's surface, a generated shape). The source is a
 * plain indexed mesh with one colour per vertex; a build cuts it into
 * clusters of at most R3D_DYNAMIC_CLUSTER_TRIANGLES triangles, in source
 * order, each owning copies of the vertices it uses, and hangs them off a
 * two-level tree. The result is an ordinary r3d_lit_mesh_t the raster
 * draws, valid until the next build.
 *
 * The caller owns one block; nothing here allocates.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "math/linear/vec3f.h"
#include "render/r3d_lit_mesh.h"

/* The bake's default meshlet size (geometry.meshlet_triangles). */
#define R3D_DYNAMIC_CLUSTER_TRIANGLES 32
/* Clusters under one leaf node. */
#define R3D_DYNAMIC_LEAF_CLUSTERS     8

typedef struct {
    const vec3f_t* positions;   /* model units */
    const uint8_t (*colors)[3]; /* sRGB, one per vertex */
    int vertex_count;
    const uint16_t (*triangles)[3]; /* counter-clockwise seen from the front */
    int triangle_count;
    bool double_sided;
} r3d_dynamic_source_t;

typedef struct {
    int vertices;  /* output vertices, counting the copies clusters make */
    int triangles; /* output triangles */
    int sources;   /* source vertices */
} r3d_dynamic_capacity_t;

typedef struct {
    int16_t (*positions)[3];
    uint8_t (*colors)[3];
    uint16_t (*triangles)[3];
    r3d_lit_cluster_t* clusters;
    r3d_lit_node_t* nodes;
    int16_t* remap; /* a source vertex's index in the open cluster, or -1 */
    r3d_dynamic_capacity_t capacity;
    int cluster_capacity, node_capacity;
    r3d_lit_mesh_t mesh;
} r3d_dynamic_mesh_t;

size_t r3d_dynamic_mesh_bytes(r3d_dynamic_capacity_t capacity);

/* Carves `block`, at malloc alignment and r3d_dynamic_mesh_bytes() long.
 * Positions are stored at `position_scale` ticks per model unit. False when
 * the block is short or a capacity is out of range. Starts empty. */
bool r3d_dynamic_mesh_init(r3d_dynamic_mesh_t* m, void* block, size_t bytes, r3d_dynamic_capacity_t capacity,
                           int position_scale);

/* Rebuilds the mesh from `src`. False when it did not fit: the mesh then
 * holds the triangles that did, in order. Positions past int16 ticks clamp.
 * An empty source leaves no clusters and no nodes: nothing to draw. */
bool r3d_dynamic_mesh_build(r3d_dynamic_mesh_t* m, const r3d_dynamic_source_t* src);
