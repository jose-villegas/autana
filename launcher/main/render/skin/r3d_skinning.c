#include "render/skin/r3d_skinning.h"
#include <assert.h>
#include <limits.h>
#include <string.h>
#include "math/scalar/mathf_round.h"
#include "math/scalar/mathi.h"
#include "render/skin/r3d_skin_light_internal.h"

static size_t
cluster_offset(const r3d_lit_mesh_t* bind) {
    const size_t vertex_bytes = (size_t)bind->vertex_count * sizeof(int16_t[3]);
    return mathi_size_ceil(vertex_bytes, _Alignof(r3d_lit_cluster_t));
}

static size_t
node_offset(const r3d_lit_mesh_t* bind) {
    return mathi_size_ceil(cluster_offset(bind) + ((size_t)bind->cluster_count * sizeof(r3d_lit_cluster_t)),
                           _Alignof(r3d_lit_node_t));
}

size_t
r3d_skin_output_bytes(const r3d_lit_mesh_t* bind) {
    return node_offset(bind) + ((size_t)bind->node_count * sizeof(r3d_lit_node_t));
}

bool
r3d_skin_output_init(const r3d_lit_mesh_t* bind, void* block, size_t bytes, r3d_skin_output_t* out) {
    *out = (r3d_skin_output_t){0};
    if (block == NULL || bytes < r3d_skin_output_bytes(bind)) {
        return false;
    }
    uint8_t* p = block;
    out->positions = (int16_t(*)[3])(void*)p;
    out->clusters = (r3d_lit_cluster_t*)(void*)(p + cluster_offset(bind));
    out->nodes = (r3d_lit_node_t*)(void*)(p + node_offset(bind));
    memcpy(out->clusters, bind->clusters, (size_t)bind->cluster_count * sizeof *out->clusters);
    memcpy(out->nodes, bind->nodes, (size_t)bind->node_count * sizeof *out->nodes);
    out->mesh = *bind;
    out->mesh.positions = (const int16_t(*)[3])out->positions;
    out->mesh.colors = bind->colors;
    out->mesh.clusters = out->clusters;
    out->mesh.nodes = out->nodes;
    return true;
}

enum { POSITION_AXES = 3, MATRIX_COLUMNS = 4 };

void
r3d_skin_palette(const r3d_skin_t* skin, const mat4f_t* model, int position_scale, mat4f_t* palette) {
    assert(position_scale > 0);
    for (int j = 0; j < skin->joint_count; j++) {
        mat4f_t inverse = mat4f_identity();
        memcpy(inverse.m, skin->inverse_binds[j], sizeof skin->inverse_binds[j]);
        palette[j] = mat4f_mul(model[j], inverse);
        for (int axis = 0; axis < POSITION_AXES; axis++) {
            palette[j].m[axis][POSITION_AXES] *= (float)position_scale;
        }
    }
}

static void
empty_bounds(int16_t lo[POSITION_AXES], int16_t hi[POSITION_AXES]) {
    for (int a = 0; a < POSITION_AXES; a++) {
        lo[a] = INT16_MAX;
        hi[a] = INT16_MIN;
    }
}

static void
grow_bounds(int16_t lo[POSITION_AXES], int16_t hi[POSITION_AXES], const int16_t from_lo[POSITION_AXES],
            const int16_t from_hi[POSITION_AXES]) {
    for (int a = 0; a < POSITION_AXES; a++) {
        lo[a] = (int16_t)(from_lo[a] < lo[a] ? from_lo[a] : lo[a]);
        hi[a] = (int16_t)(from_hi[a] > hi[a] ? from_hi[a] : hi[a]);
    }
}

static inline __attribute__((always_inline)) void
skin_vertex(const uint8_t* record, int influences, const mat4f_t* palette, const int16_t bind[POSITION_AXES],
            int16_t out[POSITION_AXES], vec3f_t* normal) {
    vec3f_t rows[POSITION_AXES];
    float posed[POSITION_AXES] = {0};
    for (int i = 0; i < influences; i++) {
        const mat4f_t* joint = &palette[record[i]];
        const float weight = (float)record[influences + i] * R3D_SKIN_WEIGHT_RECIP;
        for (int axis = 0; axis < POSITION_AXES; axis++) {
            const float position = (joint->m[axis][0] * (float)bind[0]) + (joint->m[axis][1] * (float)bind[1])
                                   + (joint->m[axis][2] * (float)bind[2]) + joint->m[axis][POSITION_AXES];
            posed[axis] += position * weight;
            if (normal != NULL) {
                const vec3f_t row =
                    vec3f_scale((vec3f_t){joint->m[axis][0], joint->m[axis][1], joint->m[axis][2]}, weight);
                rows[axis] = i == 0 ? row : vec3f_add(rows[axis], row);
            }
        }
    }
    if (normal != NULL) {
        const int8_t* packed = (const int8_t*)(record + 2 * influences);
        const vec3f_t n = {packed[0] * R3D_SKIN_NORMAL_RECIP, packed[1] * R3D_SKIN_NORMAL_RECIP,
                           packed[2] * R3D_SKIN_NORMAL_RECIP};
        *normal = (vec3f_t){vec3f_dot(rows[0], n), vec3f_dot(rows[1], n), vec3f_dot(rows[2], n)};
    }
    for (int a = 0; a < POSITION_AXES; a++) {
        out[a] = (int16_t)mathf_round_i32(mathf_clamp_finite(posed[a], (float)INT16_MIN, (float)INT16_MAX));
    }
}

static inline __attribute__((always_inline)) void
skin_range(const r3d_skin_t* skin, const r3d_lit_mesh_t* bind, const mat4f_t* palette, int first, int count,
           r3d_skin_output_t* out, int influences, const r3d_skin_lights_t* lights, bool renormalise,
           uint8_t (*light)[3]) {
    for (int c = first; c < first + count; c++) {
        r3d_lit_cluster_t* cluster = &out->clusters[c];
        empty_bounds(cluster->lo, cluster->hi);
        const int end = cluster->vertex_first + cluster->vertex_count;
        for (int v = cluster->vertex_first; v < end; v++) {
            vec3f_t normal;
            skin_vertex(skin->vertices + ((size_t)v * skin->vertex_stride), influences, palette, bind->positions[v],
                        out->positions[v], lights != NULL ? &normal : NULL);
            if (lights != NULL) {
                if (renormalise) {
                    normal = light_renormalise(normal);
                }
                light_evaluate(normal, lights, light[v]);
            }
            grow_bounds(cluster->lo, cluster->hi, out->positions[v], out->positions[v]);
        }
    }
}

static void
skin_two(const r3d_skin_t* skin, const r3d_lit_mesh_t* bind, const mat4f_t* palette, int first, int count,
         r3d_skin_output_t* out) {
    skin_range(skin, bind, palette, first, count, out, R3D_SKIN_INFLUENCES_MIN, NULL, false, NULL);
}

static void
skin_four(const r3d_skin_t* skin, const r3d_lit_mesh_t* bind, const mat4f_t* palette, int first, int count,
          r3d_skin_output_t* out) {
    skin_range(skin, bind, palette, first, count, out, R3D_SKIN_INFLUENCES_MAX, NULL, false, NULL);
}

void
r3d_skin_clusters(const r3d_skin_t* skin, const r3d_lit_mesh_t* bind, const mat4f_t* palette, int first, int count,
                  r3d_skin_output_t* out) {
    check_range(skin, bind, first, count);
    if (skin->influences == R3D_SKIN_INFLUENCES_MIN) {
        skin_two(skin, bind, palette, first, count, out);
    } else {
        skin_four(skin, bind, palette, first, count, out);
    }
}

#define LIT_RANGE(name, influences)                                                                                    \
    static void name(const r3d_skin_t* skin, const r3d_lit_mesh_t* bind, const mat4f_t* palette, int first, int count, \
                     r3d_skin_output_t* out, const r3d_skin_lights_t* lights, bool renormalise, uint8_t(*light)[3]) {  \
        skin_range(skin, bind, palette, first, count, out, influences, lights, renormalise, light);                    \
    }
LIT_RANGE(lit_two, R3D_SKIN_INFLUENCES_MIN)
LIT_RANGE(lit_four, R3D_SKIN_INFLUENCES_MAX)

void
r3d_skin_clusters_lit(const r3d_skin_t* skin, const r3d_lit_mesh_t* bind, const mat4f_t* palette, int first, int count,
                      r3d_skin_output_t* out, const r3d_skin_lights_t* lights, bool renormalise, uint8_t (*light)[3]) {
    assert(lights != NULL);
    assert(lights->count >= 0 && lights->count <= R3D_SKIN_LIGHT_MAX);
    check_range(skin, bind, first, count);
    if (skin->influences == R3D_SKIN_INFLUENCES_MIN) {
        lit_two(skin, bind, palette, first, count, out, lights, renormalise, light);
    } else {
        lit_four(skin, bind, palette, first, count, out, lights, renormalise, light);
    }
}

void
r3d_skin_refit_nodes(r3d_skin_output_t* out) {
    for (int n = out->mesh.node_count - 1; n >= 0; n--) {
        r3d_lit_node_t* node = &out->nodes[n];
        empty_bounds(node->lo, node->hi);
        for (int i = node->first; i < node->first + node->count; i++) {
            const int16_t* lo = node->leaf ? out->clusters[i].lo : out->nodes[i].lo;
            const int16_t* hi = node->leaf ? out->clusters[i].hi : out->nodes[i].hi;
            grow_bounds(node->lo, node->hi, lo, hi);
        }
    }
}
