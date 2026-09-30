/*
 * r3d_lit_mesh, a triangle mesh whose lighting was baked into one sRGB colour
 * per vertex, split into meshlets: compact clusters of a few dozen triangles.
 * A renderer culls a cluster's vertices and triangles together. Each cluster
 * owns a contiguous range of both arrays, and its triangles index only its
 * own vertices. The clusters are the leaves of a tree rooted at nodes[0], so
 * a whole subtree is culled with one box test.
 */
#pragma once

#include <stdbool.h>
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

typedef struct {
    const int16_t (*positions)[3];  /* ticks; position_scale ticks per model unit */
    const uint8_t (*colors)[3];     /* NULL when face_colors carries the lighting */
    const uint16_t (*triangles)[3]; /* counter-clockwise seen from the front */
    const r3d_lit_cluster_t* clusters;
    const r3d_lit_node_t* nodes; /* children of a node sit together */
    int vertex_count, triangle_count, cluster_count, node_count;
    int position_scale;
    const uint16_t* face_colors; /* target-format RGB565, one per triangle, or NULL */
} r3d_lit_mesh_t;
