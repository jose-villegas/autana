/*
 * r3d_lit_mesh, a triangle mesh whose lighting was baked into one sRGB colour
 * per vertex, split into meshlets: compact clusters of a few dozen triangles.
 * A renderer culls a cluster's vertices and triangles together. Each cluster
 * owns a contiguous range of both arrays, and its triangles index only its
 * own vertices. The clusters are the leaves of a tree rooted at nodes[0], so
 * a whole subtree is culled with one box test.
 *
 * A mesh is an asset-pack entry (docs/render/Mesh-Import.md): its arrays sit at
 * offsets inside the entry, and r3d_lit_mesh_t is a view built once from
 * them, so the renderer reads pointers as it always did and nothing is copied.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "asset/asset_pack.h"
#include "math/geometry/spheref.h"

typedef struct {
    uint16_t vertex_first, vertex_count;
    uint16_t triangle_first, triangle_count;
    box3s_t bounds; /* bounds of its vertices, position ticks */
    bool double_sided;
} r3d_lit_cluster_t;

typedef struct {
    box3s_t bounds; /* bounds of everything below, position ticks */
    uint16_t first; /* first child node, or a leaf's first cluster */
    uint8_t count;  /* children, or a leaf's clusters */
    bool leaf;
} r3d_lit_node_t;

/* Asset records are read in place; these offsets are the packed file contract. */
_Static_assert(sizeof(r3d_lit_cluster_t) == 22, "r3d_lit_cluster_t asset size");
_Static_assert(offsetof(r3d_lit_cluster_t, vertex_first) == 0, "r3d_lit_cluster_t vertex_first asset offset");
_Static_assert(offsetof(r3d_lit_cluster_t, vertex_count) == 2, "r3d_lit_cluster_t vertex_count asset offset");
_Static_assert(offsetof(r3d_lit_cluster_t, triangle_first) == 4, "r3d_lit_cluster_t triangle_first asset offset");
_Static_assert(offsetof(r3d_lit_cluster_t, triangle_count) == 6, "r3d_lit_cluster_t triangle_count asset offset");
_Static_assert(offsetof(r3d_lit_cluster_t, bounds) == 8, "r3d_lit_cluster_t bounds asset offset");
_Static_assert(offsetof(r3d_lit_cluster_t, bounds.lo) == 8, "r3d_lit_cluster_t bounds.lo asset offset");
_Static_assert(offsetof(r3d_lit_cluster_t, bounds.hi) == 14, "r3d_lit_cluster_t bounds.hi asset offset");
_Static_assert(offsetof(r3d_lit_cluster_t, double_sided) == 20, "r3d_lit_cluster_t double_sided asset offset");
_Static_assert(sizeof(r3d_lit_node_t) == 16, "r3d_lit_node_t asset size");
_Static_assert(offsetof(r3d_lit_node_t, bounds) == 0, "r3d_lit_node_t bounds asset offset");
_Static_assert(offsetof(r3d_lit_node_t, bounds.lo) == 0, "r3d_lit_node_t bounds.lo asset offset");
_Static_assert(offsetof(r3d_lit_node_t, bounds.hi) == 6, "r3d_lit_node_t bounds.hi asset offset");
_Static_assert(offsetof(r3d_lit_node_t, first) == 12, "r3d_lit_node_t first asset offset");
_Static_assert(offsetof(r3d_lit_node_t, count) == 14, "r3d_lit_node_t count asset offset");
_Static_assert(offsetof(r3d_lit_node_t, leaf) == 15, "r3d_lit_node_t leaf asset offset");

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

/* The asset type of a lit mesh entry. */
#define R3D_LIT_MESH_ASSET ASSET_TYPE('L', 'M', 'S', 'H')

/* Fills `mesh` with pointers into `asset`, which must outlive it, after
 * checking every array and every cluster and node range against the entry.
 * On failure `mesh` is zeroed and the status says what was wrong. */
asset_status_t r3d_lit_mesh_from_asset(const asset_view_t* asset, r3d_lit_mesh_t* mesh);

/* The same for the lit mesh entry named `id` in `pack`. */
asset_status_t r3d_lit_mesh_open(const asset_pack_t* pack, const char* id, r3d_lit_mesh_t* mesh);

/* A sphere holding every vertex, in model units: centred on the box of them
 * (the root node's bounds), its radius the furthest vertex from that centre. */
spheref_t r3d_lit_mesh_bounding_sphere(const r3d_lit_mesh_t* mesh);
