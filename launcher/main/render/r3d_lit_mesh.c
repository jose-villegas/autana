#include "render/r3d_lit_mesh.h"

#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <string.h>
#include "asset/asset_bytes.h"
#include "asset/asset_entry.h"
#include "render/lcol_format.h"

/* The entry's bytes are the structs' own layout, written by lit_mesh.py. */
_Static_assert(sizeof(r3d_lit_cluster_t) == 22 && offsetof(r3d_lit_cluster_t, bounds) == 8
                   && offsetof(r3d_lit_cluster_t, double_sided) == 20,
               "r3d_lit_cluster_t is not the layout lit_mesh.py writes");
_Static_assert(sizeof(r3d_lit_node_t) == 16 && offsetof(r3d_lit_node_t, first) == 12
                   && offsetof(r3d_lit_node_t, leaf) == 15,
               "r3d_lit_node_t is not the layout lit_mesh.py writes");
_Static_assert(sizeof(r3d_lit_cluster_lod_t) == 4 && sizeof(r3d_lit_lod_group_t) == 10
                   && offsetof(r3d_lit_lod_group_t, error) == 8,
               "the LOD records are not the layout lit_mesh.py writes");

enum {
    HEADER_WORDS = 14,
    ENTRY_ALIGNMENT = 4,
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
    COUNT_LOD_GROUP,
    AT_CLUSTER_LOD,
    AT_LOD_GROUPS,
};

_Static_assert(HEADER_WORDS * sizeof(uint32_t) == R3D_LIT_MESH_HEADER_BYTES, "the header is its words");

#define VERTEX_LIMIT 65535U /* triangles index vertices with 16 bits */

static uint32_t
word(const uint8_t* data, int index) {
    return asset_read_u32(data + ((size_t)index * sizeof(uint32_t)));
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
clusters_cover_vertices(const r3d_lit_mesh_t* mesh) {
    int end = 0;
    for (int c = 0; c < mesh->cluster_count; c++) {
        const r3d_lit_cluster_t* cluster = &mesh->clusters[c];
        if (cluster->vertex_first != end) {
            return false;
        }
        end = cluster->vertex_first + cluster->vertex_count;
    }
    return end == mesh->vertex_count;
}

static bool
nodes_are_inside(const r3d_lit_mesh_t* mesh) {
    for (int n = 0; n < mesh->node_count; n++) {
        const r3d_lit_node_t* node = &mesh->nodes[n];
        const int limit = node->leaf ? mesh->cluster_count : mesh->node_count;
        if ((int)node->first + node->count > limit) {
            return false;
        }
        /* An inner node's children come after it, so a walk down the tree
         * always ends, and they fit the walk's ordering. */
        if (!node->leaf && ((int)node->first <= n || node->count > R3D_LIT_NODE_CHILDREN_MAX)) {
            return false;
        }
    }
    return true;
}

/* A group's id names a group, or none. */
static bool
lod_id_fits(uint16_t id, int group_count) {
    return id == R3D_LOD_NONE || id < group_count;
}

/* The pick's ordering (render/r3d_lod.h): a cluster's own group, the one
 * simplified into it, sits inside its parent with R3D_LOD_ENCLOSE_MARGIN to
 * spare and has no larger error, so a parent never projects smaller. */
static bool
lod_is_ordered(const r3d_lit_mesh_t* mesh) {
    for (int c = 0; c < mesh->cluster_count; c++) {
        const r3d_lit_cluster_lod_t lod = mesh->cluster_lod[c];
        if (!lod_id_fits(lod.own, mesh->lod_group_count) || !lod_id_fits(lod.parent, mesh->lod_group_count)) {
            return false;
        }
        if (lod.own == R3D_LOD_NONE || lod.parent == R3D_LOD_NONE) {
            continue;
        }
        const r3d_lit_lod_group_t* inner = &mesh->lod_groups[lod.own];
        const r3d_lit_lod_group_t* outer = &mesh->lod_groups[lod.parent];
        const int64_t room = (int64_t)outer->bounds.radius - inner->bounds.radius - R3D_LOD_ENCLOSE_MARGIN;
        /* Widened: two int16 ticks can differ by more than an int16 holds. A
         * widened spheres contains_ball replaces this (autana-4zwhv). */
        const int64_t dx = (int64_t)outer->bounds.centre.x - inner->bounds.centre.x;
        const int64_t dy = (int64_t)outer->bounds.centre.y - inner->bounds.centre.y;
        const int64_t dz = (int64_t)outer->bounds.centre.z - inner->bounds.centre.z;
        if (inner->error > outer->error || room < 0 || (dx * dx) + (dy * dy) + (dz * dz) > room * room) {
            return false;
        }
    }
    for (int g = 0; g < mesh->lod_group_count; g++) {
        if (mesh->lod_groups[g].bounds.radius < 0) {
            return false;
        }
    }
    return true;
}

asset_status_t
r3d_lit_mesh_from_asset(const asset_view_t* asset, r3d_lit_mesh_t* mesh) {
    *mesh = (r3d_lit_mesh_t){0};
    const asset_status_t status = asset_entry_validate(*asset, ASSET_ENTRY_UNVERSIONED, ASSET_ENTRY_UNVERSIONED,
                                                       HEADER_WORDS * sizeof(uint32_t), ENTRY_ALIGNMENT);
    if (status != ASSET_OK) {
        return status;
    }
    const uint32_t vertices = word(asset->data, COUNT_VERTEX);
    const uint32_t triangles = word(asset->data, COUNT_TRIANGLE);
    const uint32_t clusters = word(asset->data, COUNT_CLUSTER);
    const uint32_t nodes = word(asset->data, COUNT_NODE);
    const uint32_t colors_at = word(asset->data, AT_COLORS);
    const uint32_t face_at = word(asset->data, AT_FACE_COLORS);
    const uint32_t groups = word(asset->data, COUNT_LOD_GROUP);
    const uint32_t cluster_lod_at = word(asset->data, AT_CLUSTER_LOD);
    const uint32_t groups_at = word(asset->data, AT_LOD_GROUPS);
    if (vertices > VERTEX_LIMIT || triangles > VERTEX_LIMIT || clusters > VERTEX_LIMIT || nodes > VERTEX_LIMIT
        || groups >= R3D_LOD_NONE) {
        return ASSET_ERR_BOUNDS;
    }
    /* Levels come whole: the groups and every cluster's record, or neither. */
    if ((groups == 0) != (cluster_lod_at == 0) || (groups == 0) != (groups_at == 0)) {
        return ASSET_ERR_FORMAT;
    }
    const uint32_t scale = word(asset->data, POSITION_SCALE);
    /* Colourless prototypes receive their lighting at scene open. */
    if (clusters == 0 || nodes == 0 || (colors_at != 0 && face_at != 0) || scale == 0 || scale > INT_MAX) {
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
        .position_scale = (int)scale,
        .face_colors = face_at == 0 ? NULL : array_at(asset, face_at, triangles, 2),
        .cluster_lod = groups == 0 ? NULL : array_at(asset, cluster_lod_at, clusters, sizeof(r3d_lit_cluster_lod_t)),
        .lod_groups = groups == 0 ? NULL : array_at(asset, groups_at, groups, sizeof(r3d_lit_lod_group_t)),
        .lod_group_count = (int)groups,
    };
    const bool colours_fit = (colors_at == 0 || built.colors != NULL) && (face_at == 0 || built.face_colors != NULL);
    const bool levels_fit = groups == 0 || (built.cluster_lod != NULL && built.lod_groups != NULL);
    if (built.positions == NULL || built.triangles == NULL || built.clusters == NULL || built.nodes == NULL
        || !colours_fit || !levels_fit || !clusters_are_inside(&built) || !nodes_are_inside(&built)) {
        return ASSET_ERR_BOUNDS;
    }
    if (!clusters_cover_vertices(&built) || (groups != 0 && !lod_is_ordered(&built))) {
        return ASSET_ERR_FORMAT;
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

static asset_status_t
r3d_lit_mesh_colour_name(const char* mesh_id, char out[ASSET_NAME_MAX]) {
    const size_t length = strlen(mesh_id);
    if (length == 0) {
        return ASSET_ERR_FORMAT;
    }
    if (length + sizeof LCOL_NAME_SUFFIX > ASSET_NAME_MAX) {
        return ASSET_ERR_BOUNDS;
    }
    memcpy(out, mesh_id, length);
    memcpy(out + length, LCOL_NAME_SUFFIX, sizeof LCOL_NAME_SUFFIX);
    return ASSET_OK;
}

asset_status_t
r3d_lit_mesh_open_colours(const asset_pack_t* pack, const char* mesh_id, uint16_t set, r3d_lit_mesh_t* mesh) {
    char name[ASSET_NAME_MAX];
    asset_status_t status = r3d_lit_mesh_colour_name(mesh_id, name);
    if (status != ASSET_OK) {
        return status;
    }
    asset_view_t view;
    status = asset_entry_open(pack, name, LCOL_ASSET, LCOL_VERSION, LCOL_AT_VERSION, LCOL_HEADER_SIZE, ENTRY_ALIGNMENT,
                              &view);
    if (status != ASSET_OK) {
        return status;
    }
    const asset_view_t* asset = &view;
    const uint16_t encoding = asset_read_u16(asset->data + LCOL_AT_ENCODING);
    const uint32_t sets = asset_read_u32(asset->data + LCOL_AT_SET_COUNT);
    const uint32_t count = asset_read_u32(asset->data + LCOL_AT_COUNT);
    if (encoding != LCOL_ENCODING_RGB888 && encoding != LCOL_ENCODING_RGB565) {
        return ASSET_ERR_FORMAT;
    }
    const bool vertex = encoding == LCOL_ENCODING_RGB888;
    const uint32_t expected = (uint32_t)(vertex ? mesh->vertex_count : mesh->triangle_count);
    const uint32_t stride = vertex ? LCOL_RGB888_BYTES : LCOL_RGB565_BYTES;
    if (sets == 0 || set >= sets || count != expected || mesh->colors != NULL || mesh->face_colors != NULL) {
        return ASSET_ERR_FORMAT;
    }
    const uint64_t bytes = (uint64_t)count * stride;
    if (LCOL_HEADER_SIZE + (bytes * sets) != asset->size) {
        return ASSET_ERR_BOUNDS;
    }
    const void* colors = asset->data + LCOL_HEADER_SIZE + (bytes * set);
    mesh->colors = vertex ? colors : NULL;
    mesh->face_colors = vertex ? NULL : colors;
    return ASSET_OK;
}

spheref_t
r3d_lit_mesh_bounding_sphere(const r3d_lit_mesh_t* mesh) {
    const float units = 1.0F / (float)mesh->position_scale;
    const vec3f_t mid = box3f_centre(box3f_from_box3s(mesh->nodes[0].bounds, units));
    float furthest = 0.0F;
    for (int i = 0; i < mesh->vertex_count; i++) {
        const vec3f_t p = vec3f_from_vec3s(vec3s_from_array(mesh->positions[i]), units);
        const vec3f_t d = vec3f_sub(p, mid);
        furthest = fmaxf(furthest, vec3f_dot(d, d));
    }
    return (spheref_t){mid, sqrtf(furthest)};
}

box3f_t
r3d_lit_mesh_world_bounds(const r3d_lit_mesh_t* mesh, const mat4f_t* model) {
    const box3f_t box = box3f_from_box3s(mesh->nodes[0].bounds, 1.0F / (float)mesh->position_scale);
    if (model == NULL) {
        return box;
    }
    box3f_t world = box3f_empty();
    for (int k = 0; k < BOX3_CORNERS; k++) {
        world = box3f_extend(world, mat4f_apply(model, box3f_corner(box, k)));
    }
    return world;
}
