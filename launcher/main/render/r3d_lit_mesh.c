#include "render/r3d_lit_mesh.h"

#include <math.h>
#include <stddef.h>

/* The entry's bytes are the structs' own layout, written by lit_mesh.py. */
_Static_assert(sizeof(r3d_lit_cluster_t) == 22 && offsetof(r3d_lit_cluster_t, lo) == 8
                   && offsetof(r3d_lit_cluster_t, double_sided) == 20,
               "r3d_lit_cluster_t is not the layout lit_mesh.py writes");
_Static_assert(sizeof(r3d_lit_node_t) == 16 && offsetof(r3d_lit_node_t, first) == 12
                   && offsetof(r3d_lit_node_t, leaf) == 15,
               "r3d_lit_node_t is not the layout lit_mesh.py writes");

enum {
    HEADER_WORDS = 11,
    COUNT_VERTEX = 0,
    COUNT_TRIANGLE,
    COUNT_CLUSTER,
    COUNT_NODE,
    POSITION_SCALE,
    AT_POSITIONS,
    AT_COLORS,
    AT_TRIANGLES,
    AT_CLUSTERS,
    AT_NODES,
    AT_FACE_COLORS,
};

#define VERTEX_LIMIT 65535U /* triangles index vertices with 16 bits */

static uint32_t
word(const uint8_t* data, int index) {
    const uint8_t* at = data + ((size_t)index * 4U);
    return (uint32_t)at[0] | ((uint32_t)at[1] << 8) | ((uint32_t)at[2] << 16) | ((uint32_t)at[3] << 24);
}

/* A pointer to `count` items of `bytes` each at `offset`, or NULL when they
 * do not fit the entry, or sit off a 4-byte boundary the structs need. */
static const void*
array_at(const asset_view_t* asset, uint32_t offset, uint32_t count, uint32_t bytes) {
    if (offset % 4U != 0 || offset < HEADER_WORDS * 4U) {
        return NULL;
    }
    if ((uint64_t)offset + ((uint64_t)count * bytes) > asset->size) {
        return NULL;
    }
    return asset->data + offset;
}

static bool
clusters_are_inside(const r3d_lit_mesh_t* mesh) {
    for (int c = 0; c < mesh->cluster_count; c++) {
        const r3d_lit_cluster_t* cluster = &mesh->clusters[c];
        if ((int)cluster->vertex_first + cluster->vertex_count > mesh->vertex_count
            || (int)cluster->triangle_first + cluster->triangle_count > mesh->triangle_count) {
            return false;
        }
        for (int t = 0; t < cluster->triangle_count; t++) {
            for (int corner = 0; corner < 3; corner++) {
                const int vertex = mesh->triangles[cluster->triangle_first + t][corner];
                if (vertex < cluster->vertex_first || vertex >= cluster->vertex_first + cluster->vertex_count) {
                    return false;
                }
            }
        }
    }
    return true;
}

static bool
nodes_are_inside(const r3d_lit_mesh_t* mesh) {
    for (int n = 0; n < mesh->node_count; n++) {
        const r3d_lit_node_t* node = &mesh->nodes[n];
        const int limit = node->leaf ? mesh->cluster_count : mesh->node_count;
        if ((int)node->first + node->count > limit) {
            return false;
        }
        /* An inner node's children come after it, so a walk down the tree always ends. */
        if (!node->leaf && (int)node->first <= n) {
            return false;
        }
    }
    return true;
}

asset_status_t
r3d_lit_mesh_from_asset(const asset_view_t* asset, r3d_lit_mesh_t* mesh) {
    *mesh = (r3d_lit_mesh_t){0};
    if (asset->size < HEADER_WORDS * 4U) {
        return ASSET_ERR_BOUNDS;
    }
    const uint32_t vertices = word(asset->data, COUNT_VERTEX);
    const uint32_t triangles = word(asset->data, COUNT_TRIANGLE);
    const uint32_t clusters = word(asset->data, COUNT_CLUSTER);
    const uint32_t nodes = word(asset->data, COUNT_NODE);
    const uint32_t colors_at = word(asset->data, AT_COLORS);
    const uint32_t face_at = word(asset->data, AT_FACE_COLORS);
    if (vertices > VERTEX_LIMIT || triangles > VERTEX_LIMIT || clusters > VERTEX_LIMIT || nodes > VERTEX_LIMIT) {
        return ASSET_ERR_BOUNDS;
    }
    /* One colour source, a scale, and a root for the tree to start from. */
    if (clusters == 0 || nodes == 0 || (colors_at == 0) == (face_at == 0) || word(asset->data, POSITION_SCALE) == 0) {
        return ASSET_ERR_FORMAT;
    }
    r3d_lit_mesh_t built = {
        .positions = array_at(asset, word(asset->data, AT_POSITIONS), vertices, 6),
        .colors = colors_at == 0 ? NULL : array_at(asset, colors_at, vertices, 3),
        .triangles = array_at(asset, word(asset->data, AT_TRIANGLES), triangles, 6),
        .clusters = array_at(asset, word(asset->data, AT_CLUSTERS), clusters, sizeof(r3d_lit_cluster_t)),
        .nodes = array_at(asset, word(asset->data, AT_NODES), nodes, sizeof(r3d_lit_node_t)),
        .vertex_count = (int)vertices,
        .triangle_count = (int)triangles,
        .cluster_count = (int)clusters,
        .node_count = (int)nodes,
        .position_scale = (int)word(asset->data, POSITION_SCALE),
        .face_colors = face_at == 0 ? NULL : array_at(asset, face_at, triangles, 2),
    };
    const bool colours_fit = colors_at == 0 ? built.face_colors != NULL : built.colors != NULL;
    if (built.positions == NULL || built.triangles == NULL || built.clusters == NULL || built.nodes == NULL
        || !colours_fit || !clusters_are_inside(&built) || !nodes_are_inside(&built)) {
        return ASSET_ERR_BOUNDS;
    }
    *mesh = built;
    return ASSET_OK;
}

asset_status_t
r3d_lit_mesh_open(const asset_pack_t* pack, const char* id, r3d_lit_mesh_t* mesh) {
    asset_view_t asset;
    const asset_status_t status = asset_pack_find(pack, id, R3D_LIT_MESH_ASSET, &asset);
    if (status != ASSET_OK) {
        *mesh = (r3d_lit_mesh_t){0};
        return status;
    }
    return r3d_lit_mesh_from_asset(&asset, mesh);
}

void
r3d_lit_mesh_bounding_sphere(const r3d_lit_mesh_t* mesh, vec3f_t* centre, float* radius) {
    const float units = 1.0F / (float)mesh->position_scale;
    const r3d_lit_node_t* root = &mesh->nodes[0];
    const vec3f_t mid = {(float)(root->lo[0] + root->hi[0]) * 0.5F * units,
                         (float)(root->lo[1] + root->hi[1]) * 0.5F * units,
                         (float)(root->lo[2] + root->hi[2]) * 0.5F * units};
    float furthest = 0.0F;
    for (int i = 0; i < mesh->vertex_count; i++) {
        const vec3f_t p = {(float)mesh->positions[i][0] * units, (float)mesh->positions[i][1] * units,
                           (float)mesh->positions[i][2] * units};
        const vec3f_t d = vec3f_sub(p, mid);
        furthest = fmaxf(furthest, vec3f_dot(d, d));
    }
    *centre = mid;
    *radius = sqrtf(furthest);
}
