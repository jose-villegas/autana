/*
 * lit_mesh - a triangle mesh whose lighting was baked into one sRGB colour
 * per vertex, split into spatial clusters so a renderer culls a cluster's
 * vertices and triangles together. Each cluster owns a contiguous range of
 * both arrays, and its triangles index only its own vertices, counted from
 * its vertex_first. The clusters are the leaves of a tree rooted at
 * nodes[0], so a whole subtree is culled with one box test.
 *
 * A node may also carry a proxy: clusters of its own standing in for its
 * whole subtree with fewer triangles, never farther than lod_error from it.
 * A proxy keeps every edge its subtree shares with the rest of the mesh, so
 * it meets full detail, or another proxy, without a crack.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t vertex_first;
    uint16_t vertex_count;
    uint32_t triangle_first;
    uint16_t triangle_count;
    int16_t lo[3], hi[3]; /* bounds of its vertices, position ticks */
    bool double_sided;
} lit_cluster_t;

typedef struct {
    int16_t lo[3], hi[3]; /* bounds of everything below, position ticks */
    uint16_t first;       /* first child node, or a leaf's first cluster */
    uint8_t count;        /* children, or a leaf's clusters */
    bool leaf;
    uint16_t lod_first; /* the proxy's first cluster */
    uint8_t lod_count;  /* the proxy's clusters, 0 for none */
    uint16_t lod_error; /* position ticks */
} lit_node_t;

typedef struct {
    const int16_t (*positions)[3]; /* ticks; position_scale ticks per model unit */
    const uint8_t (*colors)[3];
    const uint16_t (*triangles)[3]; /* counter-clockwise seen from the front */
    const lit_cluster_t* clusters;
    const lit_node_t* nodes; /* children of a node sit together */
    int vertex_count, triangle_count, cluster_count, node_count;
    int position_scale;
} lit_mesh_t;
