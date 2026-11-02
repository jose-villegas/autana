/*
 * r3d_lit_mesh, a triangle mesh whose lighting was baked into one sRGB colour
 * per vertex, split into meshlets: compact clusters of a few dozen triangles.
 * A renderer culls a cluster's vertices and triangles together. Each cluster
 * owns a contiguous range of both arrays, and its triangles index only its
 * own vertices. The clusters are the leaves of a tree rooted at nodes[0], so
 * a whole subtree is culled with one box test.
 *
 * A mesh is an asset-pack entry (docs/assets/formats/LMSH.md): its arrays sit at
 * offsets inside the entry, and r3d_lit_mesh_t is a view built once from
 * them, so the renderer reads pointers as it always did and nothing is copied.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "asset/asset_pack.h"
#include "math/geometry/box3f.h"
#include "math/geometry/box3s.h"
#include "math/geometry/spheref.h"
#include "math/geometry/spheres.h"
#include "math/linear/mat4f.h"

typedef struct {
    uint16_t vertex_first, vertex_count;
    uint16_t triangle_first, triangle_count;
    box3s_t bounds; /* bounds of its vertices, position ticks */
    bool double_sided;
} r3d_lit_cluster_t;

/* The tree lit_mesh.py writes: an octree, so a node has at most eight
 * children, at most its MAX_DEPTH levels below the root. */
#define R3D_LIT_NODE_CHILDREN_MAX 8
#define R3D_LIT_TREE_DEPTH_MAX    10
/* What a depth-first walk of the tree holds pending at most: each level's
 * unvisited siblings, with room to spare. */
#define R3D_LIT_WALK_STACK_MAX    256
_Static_assert((R3D_LIT_TREE_DEPTH_MAX * (R3D_LIT_NODE_CHILDREN_MAX - 1)) + 1 <= R3D_LIT_WALK_STACK_MAX,
               "a deepest walk fits its stack");

typedef struct {
    box3s_t bounds; /* bounds of everything below, position ticks */
    uint16_t first; /* first child node, or a leaf's first cluster */
    uint8_t count;  /* children, or a leaf's clusters */
    bool leaf;
} r3d_lit_node_t;

/* Levels of detail (docs/plans/Cluster-LOD.md): a mesh may carry coarser
 * clusters beside its finest ones, each simplified from a group of finer
 * clusters with the group's border kept. A group is drawn coarse when its
 * error, projected, is small enough (render/r3d_lod.h); then its clusters
 * give way to the ones simplified from it. Both sides of a switch read the
 * one group record, so neighbours never crack. */
#define R3D_LOD_NONE           0xFFFFU /* own: a finest cluster; parent: a group never drawn coarse */
/* Position ticks a parent group's bounds keep beyond those of each group
 * simplified into it, so float rounding in the pick cannot reorder them. */
#define R3D_LOD_ENCLOSE_MARGIN 1

typedef struct {
    uint16_t own;    /* the group simplified into this cluster */
    uint16_t parent; /* the group this cluster belongs to */
} r3d_lit_cluster_lod_t;

typedef struct {
    spheres_t bounds; /* position ticks; holds the bounds of every group simplified into it */
    uint16_t error;   /* position ticks; at least that of every group simplified into it */
} r3d_lit_lod_group_t;

typedef struct {
    const int16_t (*positions)[3];  /* ticks; position_scale ticks per model unit */
    const uint8_t (*colors)[3];     /* NULL for face colours or a colourless prototype */
    const uint16_t (*triangles)[3]; /* counter-clockwise seen from the front */
    const r3d_lit_cluster_t* clusters;
    const r3d_lit_node_t* nodes; /* children of a node sit together */
    int vertex_count, triangle_count, cluster_count, node_count;
    int position_scale;
    const uint16_t* face_colors;              /* target-format RGB565, one per triangle, or NULL */
    const r3d_lit_cluster_lod_t* cluster_lod; /* one per cluster, or NULL: every cluster is finest */
    const r3d_lit_lod_group_t* lod_groups;
    int lod_group_count;
} r3d_lit_mesh_t;

/* The asset type of a lit mesh entry, and the bytes of its header. */
#define R3D_LIT_MESH_ASSET        ASSET_TYPE('L', 'M', 'S', 'H')
#define R3D_LIT_MESH_HEADER_BYTES 56

/* Fills `mesh` with pointers into `asset`, which must outlive it, after
 * checking every array and every cluster and node range against the entry.
 * On failure `mesh` is zeroed and the status says what was wrong. */
asset_status_t r3d_lit_mesh_from_asset(const asset_view_t* asset, r3d_lit_mesh_t* mesh);

/* The same for the lit mesh entry named `id` in `pack`. */
asset_status_t r3d_lit_mesh_open(const asset_pack_t* pack, const char* id, r3d_lit_mesh_t* mesh);

/* Colour ownership: docs/assets/formats/LCOL.md. */
asset_status_t r3d_lit_mesh_open_colours(const asset_pack_t* pack, const char* mesh_id, uint16_t set,
                                         r3d_lit_mesh_t* mesh);

/* Recomputes every node's bounds from its children's. An inner node's
 * children come after it, so walking from the last node back meets every
 * child before its parent. For a mesh whose vertices move. */
static inline void
r3d_lit_mesh_refit_nodes(r3d_lit_node_t* nodes, int node_count, const r3d_lit_cluster_t* clusters) {
    for (int n = node_count - 1; n >= 0; n--) {
        r3d_lit_node_t* node = &nodes[n];
        node->bounds = box3s_empty();
        for (int i = node->first; i < node->first + node->count; i++) {
            node->bounds = box3s_union(node->bounds, node->leaf ? clusters[i].bounds : nodes[i].bounds);
        }
    }
}

/* A sphere holding every vertex, in model units: centred on the box of them
 * (the root node's bounds), its radius the furthest vertex from that centre. */
spheref_t r3d_lit_mesh_bounding_sphere(const r3d_lit_mesh_t* mesh);

/* The box of the root node's bounds carried through `model` (NULL: as it is),
 * in world units: it holds every vertex where the instance sits. */
box3f_t r3d_lit_mesh_world_bounds(const r3d_lit_mesh_t* mesh, const mat4f_t* model);
