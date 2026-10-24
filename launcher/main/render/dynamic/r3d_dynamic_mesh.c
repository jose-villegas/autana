#include "render/dynamic/r3d_dynamic_mesh.h"

#include <assert.h>
#include <string.h>

#include "math/geometry/box3s.h"
#include "math/scalar/mathf_round.h"
#include "math/scalar/mathi.h"

#pragma GCC diagnostic error "-Wdouble-promotion"

#define NOT_MAPPED       (-1)
/* Indices are uint16_t, the remap int16_t. */
#define INDEX_LIMIT      UINT16_MAX
#define SOURCE_LIMIT     INT16_MAX
/* A cluster's vertices at most: three a triangle, none shared. */
#define CLUSTER_VERTICES (3 * R3D_DYNAMIC_CLUSTER_TRIANGLES)
/* Enough for every cluster an index can name: 8^6 leaves' worth. */
#define TREE_LEVELS_MAX  6

static int
clusters_for(int triangles) {
    return (triangles + R3D_DYNAMIC_CLUSTER_TRIANGLES - 1) / R3D_DYNAMIC_CLUSTER_TRIANGLES;
}

/* One more than the triangles fill, so a full build still leaves a spare. */
static int
cluster_capacity_for(int triangles) {
    return clusters_for(triangles) + 1;
}

static int
leaves_for(int clusters) {
    return (clusters + R3D_DYNAMIC_LEAF_CLUSTERS - 1) / R3D_DYNAMIC_LEAF_CLUSTERS;
}

/* Nodes per level of the tree, leaves first, root last, each inner node
 * over at most R3D_LIT_INNER_CHILDREN_MAX of the level below. A single leaf
 * is its own root. Returns the levels. */
static int
levels_for(int clusters, int counts[TREE_LEVELS_MAX]) {
    int n = leaves_for(clusters);
    int levels = 0;
    counts[levels++] = n;
    while (n > 1) {
        assert(levels < TREE_LEVELS_MAX);
        n = (n + R3D_LIT_INNER_CHILDREN_MAX - 1) / R3D_LIT_INNER_CHILDREN_MAX;
        counts[levels++] = n;
    }
    return levels;
}

static int
nodes_for(int clusters) {
    int counts[TREE_LEVELS_MAX];
    const int levels = levels_for(clusters, counts);
    int total = 0;
    for (int l = 0; l < levels; l++) {
        total += counts[l];
    }
    return total;
}

static bool
capacity_ok(r3d_dynamic_capacity_t c) {
    return c.vertices > 0 && c.vertices <= INDEX_LIMIT && c.triangles > 0 && c.triangles <= INDEX_LIMIT && c.sources > 0
           && c.sources <= SOURCE_LIMIT;
}

static size_t
align_up(size_t n) {
    return mathi_size_ceil(n, _Alignof(max_align_t));
}

static size_t
layout(r3d_dynamic_capacity_t c, size_t offsets[6]) {
    const size_t sizes[6] = {
        (size_t)c.vertices * sizeof(int16_t[3]),
        (size_t)c.vertices * sizeof(uint8_t[3]),
        (size_t)c.triangles * sizeof(uint16_t[3]),
        (size_t)cluster_capacity_for(c.triangles) * sizeof(r3d_lit_cluster_t),
        (size_t)nodes_for(cluster_capacity_for(c.triangles)) * sizeof(r3d_lit_node_t),
        (size_t)c.sources * sizeof(int16_t),
    };
    size_t at = 0;
    for (int i = 0; i < 6; i++) {
        offsets[i] = at;
        at += align_up(sizes[i]);
    }
    return at;
}

r3d_dynamic_capacity_t
r3d_dynamic_capacity_for(int sources, int triangles) {
    return (r3d_dynamic_capacity_t){mathi_min(3 * triangles, INDEX_LIMIT), triangles, sources};
}

size_t
r3d_dynamic_mesh_bytes(r3d_dynamic_capacity_t capacity) {
    size_t offsets[6];
    return capacity_ok(capacity) ? layout(capacity, offsets) : 0;
}

bool
r3d_dynamic_mesh_init(r3d_dynamic_mesh_t* m, void* block, size_t bytes, r3d_dynamic_capacity_t capacity,
                      int position_scale) {
    memset(m, 0, sizeof *m);
    size_t at[6];
    if (!capacity_ok(capacity) || block == NULL || position_scale <= 0 || bytes < layout(capacity, at)) {
        return false;
    }
    uint8_t* p = block;
    m->positions = (int16_t (*)[3])(void*)(p + at[0]);
    m->colors = (uint8_t (*)[3])(void*)(p + at[1]);
    m->triangles = (uint16_t (*)[3])(void*)(p + at[2]);
    m->clusters = (r3d_lit_cluster_t*)(void*)(p + at[3]);
    m->nodes = (r3d_lit_node_t*)(void*)(p + at[4]);
    m->remap = (int16_t*)(void*)(p + at[5]);
    m->capacity = capacity;
    m->cluster_capacity = cluster_capacity_for(capacity.triangles);
    m->node_capacity = nodes_for(m->cluster_capacity);
    for (int i = 0; i < capacity.sources; i++) {
        m->remap[i] = NOT_MAPPED;
    }
    m->mesh = (r3d_lit_mesh_t){
        .positions = (const int16_t (*)[3])m->positions,
        .colors = (const uint8_t (*)[3])m->colors,
        .triangles = (const uint16_t (*)[3])m->triangles,
        .clusters = m->clusters,
        .nodes = m->nodes,
        .position_scale = position_scale,
    };
    m->mesh.vertex_count = capacity.vertices;
    m->mesh.cluster_count = m->cluster_capacity;
    return true;
}

typedef struct {
    r3d_dynamic_mesh_t* m;
    const r3d_dynamic_source_t* src;
    r3d_lit_cluster_t* open;
    int sources[CLUSTER_VERTICES]; /* the open cluster's vertices, as source indices */
    float scale;
} builder_t;

static void
close_cluster(builder_t* b) {
    if (b->open == NULL) {
        return;
    }
    for (int i = 0; i < b->open->vertex_count; i++) {
        b->m->remap[b->sources[i]] = NOT_MAPPED;
    }
    b->open = NULL;
}

static void
open_cluster(builder_t* b) {
    r3d_dynamic_mesh_t* m = b->m;
    b->open = &m->clusters[m->mesh.cluster_count++];
    *b->open = (r3d_lit_cluster_t){
        .vertex_first = (uint16_t)m->mesh.vertex_count,
        .triangle_first = (uint16_t)m->mesh.triangle_count,
        .bounds = box3s_empty(),
        .double_sided = b->src->double_sided,
    };
}

/* The open cluster's copy of source vertex `s`, made on first use. */
static uint16_t
vertex_for(builder_t* b, int s) {
    r3d_dynamic_mesh_t* m = b->m;
    if (m->remap[s] == NOT_MAPPED) {
        const int at = m->mesh.vertex_count++;
        const vec3f_t p = vec3f_scale(b->src->positions[s], b->scale);
        const vec3s_t ticks = {mathf_round_i16(p.x), mathf_round_i16(p.y), mathf_round_i16(p.z)};
        memcpy(m->positions[at], &ticks, sizeof m->positions[at]);
        memcpy(m->colors[at], b->src->colors[s], sizeof m->colors[at]);
        b->open->bounds = box3s_extend(b->open->bounds, ticks);
        b->sources[b->open->vertex_count] = s;
        m->remap[s] = (int16_t)(at - b->open->vertex_first);
        b->open->vertex_count++;
    }
    return (uint16_t)(b->open->vertex_first + m->remap[s]);
}

/* Source vertices of triangle `t` the open cluster has no copy of yet. */
static int
new_vertices(const builder_t* b, int t) {
    const uint16_t* tri = b->src->triangles[t];
    int fresh = 0;
    for (int k = 0; k < 3; k++) {
        const bool repeat = (k > 0 && tri[k] == tri[0]) || (k > 1 && tri[k] == tri[1]);
        fresh += (b->open == NULL || b->m->remap[tri[k]] == NOT_MAPPED) && !repeat ? 1 : 0;
    }
    return fresh;
}

/* Levels laid out root first, leaves last, so an inner node's children
 * come after it, as the walk requires. */
static void
build_tree(r3d_dynamic_mesh_t* m) {
    const int clusters = m->mesh.cluster_count;
    int counts[TREE_LEVELS_MAX];
    const int levels = clusters > 0 ? levels_for(clusters, counts) : 0;
    int offset[TREE_LEVELS_MAX];
    int at = 0;
    for (int l = levels - 1; l >= 0; l--) {
        offset[l] = at;
        at += counts[l];
    }
    for (int l = 0; l < levels; l++) {
        const int fan = l == 0 ? R3D_DYNAMIC_LEAF_CLUSTERS : R3D_LIT_INNER_CHILDREN_MAX;
        const int below = l == 0 ? clusters : counts[l - 1];
        for (int i = 0; i < counts[l]; i++) {
            const int first = i * fan;
            const int count = mathi_min(below - first, fan);
            m->nodes[offset[l] + i] = (r3d_lit_node_t){
                .first = (uint16_t)(l == 0 ? first : offset[l - 1] + first),
                .count = (uint8_t)count,
                .leaf = l == 0,
            };
        }
    }
    m->mesh.node_count = at;
    r3d_lit_mesh_refit_nodes(m->nodes, m->mesh.node_count, m->clusters);
}

/* The spares: the first holds every vertex left over, the rest nothing. */
static void
pad(r3d_dynamic_mesh_t* m) {
    m->built_vertices = m->mesh.vertex_count;
    m->built_clusters = m->mesh.cluster_count;
    for (int c = m->built_clusters; c < m->cluster_capacity; c++) {
        const int first = c == m->built_clusters ? m->built_vertices : m->capacity.vertices;
        m->clusters[c] = (r3d_lit_cluster_t){
            .vertex_first = (uint16_t)first,
            .vertex_count = (uint16_t)(m->capacity.vertices - first),
            .triangle_first = (uint16_t)m->mesh.triangle_count,
            .bounds = box3s_empty(),
        };
    }
    m->mesh.vertex_count = m->capacity.vertices;
    m->mesh.cluster_count = m->cluster_capacity;
}

bool
r3d_dynamic_mesh_build(r3d_dynamic_mesh_t* m, const r3d_dynamic_source_t* src) {
    m->mesh.vertex_count = 0;
    m->mesh.triangle_count = 0;
    m->mesh.cluster_count = 0;
    m->mesh.node_count = 0;
    if (src->vertex_count > m->capacity.sources) {
        return false;
    }
    builder_t b = {.m = m, .src = src, .scale = (float)m->mesh.position_scale};
    bool fitted = true;
    for (int t = 0; t < src->triangle_count && fitted; t++) {
        if (b.open != NULL && b.open->triangle_count == R3D_DYNAMIC_CLUSTER_TRIANGLES) {
            close_cluster(&b);
        }
        if (m->mesh.vertex_count + new_vertices(&b, t) > m->capacity.vertices
            || m->mesh.triangle_count >= m->capacity.triangles
            || (b.open == NULL && m->mesh.cluster_count + 1 >= m->cluster_capacity)) {
            fitted = false;
            break;
        }
        if (b.open == NULL) {
            open_cluster(&b);
        }
        uint16_t* out = m->triangles[m->mesh.triangle_count++];
        /* Turned over for the raster: corners 0, 2, 1. */
        out[0] = vertex_for(&b, src->triangles[t][0]);
        out[2] = vertex_for(&b, src->triangles[t][1]);
        out[1] = vertex_for(&b, src->triangles[t][2]);
        b.open->triangle_count++;
    }
    close_cluster(&b);
    build_tree(m);
    pad(m);
    return fitted;
}
