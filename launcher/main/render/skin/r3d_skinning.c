#include "render/skin/r3d_skinning.h"
#include <assert.h>
#include <limits.h>
#include <string.h>
#include "math/scalar/mathf_round.h"
#include "math/scalar/mathi.h"
#include "render/r3d_skin_format.h"
#include "render/skin/r3d_skin_light_internal.h"

static inline __attribute__((always_inline)) void
check_range(const r3d_skin_t* skin, const r3d_lit_mesh_t* bind, int first, int count) {
    assert(skin->vertex_count == (uint32_t)bind->vertex_count);
    assert(first >= 0 && count >= 0 && first <= bind->cluster_count && count <= bind->cluster_count - first);
    assert(skin->influences == R3D_SKIN_INFLUENCES_MIN || skin->influences == R3D_SKIN_INFLUENCES_MAX);
}

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
    out->positions = (int16_t (*)[3])(void*)p;
    out->clusters = (r3d_lit_cluster_t*)(void*)(p + cluster_offset(bind));
    out->nodes = (r3d_lit_node_t*)(void*)(p + node_offset(bind));
    if (bind->cluster_count != 0) {
        memcpy(out->clusters, bind->clusters, (size_t)bind->cluster_count * sizeof *out->clusters);
    }
    if (bind->node_count != 0) {
        memcpy(out->nodes, bind->nodes, (size_t)bind->node_count * sizeof *out->nodes);
    }
    out->mesh = *bind;
    out->mesh.positions = (const int16_t (*)[3])out->positions;
    out->mesh.colors = bind->colors;
    out->mesh.clusters = out->clusters;
    out->mesh.nodes = out->nodes;
    return true;
}

bool
r3d_skin_normal_palette(const mat4f_t* palette, int joints, mat4f_t world, mat3f_t* normals) {
    const mat3f_t linear = mat3f_from_mat4(world);
    for (int joint = 0; joint < joints; joint++) {
        const mat3f_t matrix = mat3f_mul(linear, mat3f_from_mat4(palette[joint]));
        const mat3f_t cofactor = mat3f_cofactor(matrix);
        const float det = mat3f_determinant(matrix);
        if (det == 0.0F || !isfinite(det)) {
            return false;
        }
        const float reciprocal = 1.0F / det;
        float sum = 0.0F;
        for (int row = 0; row < MAT3F_AXES; row++) {
            for (int col = 0; col < MAT3F_AXES; col++) {
                const float value = cofactor.m[row][col] * reciprocal;
                normals[joint].m[row][col] = value;
                sum += value;
            }
        }
        if (!isfinite(sum)) {
            return false;
        }
    }
    return true;
}

enum { POSITION_AXES = 3, MATRIX_COLUMNS = 4 };

enum { LIGHT_DIRECT, LIGHT_RENORMALISED, LIGHT_TABLE };

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

static inline __attribute__((always_inline)) float
skin_weight(const uint8_t* record, int influences, int influence) {
    return (float)record[influences + influence] * R3D_SKIN_WEIGHT_RECIP;
}

static inline __attribute__((always_inline)) void
skin_blend_position_axis(const mat4f_t* joint, const int16_t bind[POSITION_AXES], int axis, float weight,
                         float* posed) {
    const float position = (joint->m[axis][0] * (float)bind[0]) + (joint->m[axis][1] * (float)bind[1])
                           + (joint->m[axis][2] * (float)bind[2]) + joint->m[axis][POSITION_AXES];
    *posed += position * weight;
}

static inline __attribute__((always_inline)) void
skin_store_position(const float posed[POSITION_AXES], int16_t out[POSITION_AXES]) {
    for (int a = 0; a < POSITION_AXES; a++) {
        out[a] = mathf_round_i16(posed[a]);
    }
}

static inline __attribute__((always_inline)) void
skin_vertex(const uint8_t* record, int influences, const mat4f_t* palette, const int16_t bind[POSITION_AXES],
            int16_t out[POSITION_AXES]) {
    float posed[POSITION_AXES] = {0};
    for (int i = 0; i < influences; i++) {
        const mat4f_t* joint = &palette[record[i]];
        const float weight = skin_weight(record, influences, i);
        for (int axis = 0; axis < POSITION_AXES; axis++) {
            skin_blend_position_axis(joint, bind, axis, weight, &posed[axis]);
        }
    }
    skin_store_position(posed, out);
}

static inline __attribute__((always_inline)) void
skin_lit_vertex(const uint8_t* record, int influences, const mat4f_t* palette, const int16_t bind[POSITION_AXES],
                int16_t out[POSITION_AXES], const mat3f_t* normals, vec3f_t* normal) {
    skin_vertex(record, influences, palette, bind, out);
    const int8_t* packed = (const int8_t*)(record + (2 * influences));
    const vec3f_t n = {(float)packed[0] * R3D_SKIN_NORMAL_RECIP, (float)packed[1] * R3D_SKIN_NORMAL_RECIP,
                       (float)packed[2] * R3D_SKIN_NORMAL_RECIP};
    float blended[POSITION_AXES] = {0};
    for (int i = 0; i < influences; i++) {
        const float weight = skin_weight(record, influences, i);
        const mat3f_t* matrix = &normals[record[i]];
        for (int axis = 0; axis < POSITION_AXES; axis++) {
            const vec3f_t row = {matrix->m[axis][0], matrix->m[axis][1], matrix->m[axis][2]};
            blended[axis] += vec3f_dot(row, n) * weight;
        }
    }
    *normal = (vec3f_t){blended[0], blended[1], blended[2]};
}

static inline __attribute__((always_inline)) void
skin_light_vertex(vec3f_t normal, const r3d_skin_lights_t* lights, const uint8_t albedo[3], uint8_t out[3], int mode) {
    if (mode == LIGHT_TABLE) {
        light_shade(r3d_skin_light_table_sample(normal, lights), lights, albedo, out);
    } else {
        light_evaluate(mode == LIGHT_RENORMALISED ? light_renormalise(normal) : normal, lights, albedo, out);
    }
}

/* The bounds of a cluster's posed vertices, in a pass after the skinning loop so
 * the bounds stay out of that loop's registers. */
static box3s_t
cluster_bounds(const int16_t (*posed)[POSITION_AXES], const r3d_lit_cluster_t* cluster) {
    box3s_t b = box3s_empty();
    const int end = cluster->vertex_first + cluster->vertex_count;
    for (int v = cluster->vertex_first; v < end; v++) {
        b = box3s_extend(b, vec3s_from_array(posed[v]));
    }
    return b;
}

static inline __attribute__((always_inline)) void
skin_range(const r3d_skin_t* skin, const r3d_lit_mesh_t* bind, const mat4f_t* palette, int first, int count,
           r3d_skin_output_t* out, int influences) {
    for (int c = first; c < first + count; c++) {
        r3d_lit_cluster_t* cluster = &out->clusters[c];
        const int end = cluster->vertex_first + cluster->vertex_count;
        for (int v = cluster->vertex_first; v < end; v++) {
            skin_vertex(skin->vertices + ((size_t)v * skin->vertex_stride), influences, palette, bind->positions[v],
                        out->positions[v]);
        }
        cluster->bounds = cluster_bounds(out->positions, cluster);
    }
}

static inline __attribute__((always_inline)) void
skin_lit_range(const r3d_skin_t* skin, const r3d_lit_mesh_t* bind, const mat4f_t* palette, int first, int count,
               r3d_skin_output_t* out, const mat3f_t* normals, const r3d_skin_lights_t* lights,
               uint8_t (*light)[R3D_SKIN_LIGHT_CHANNELS], int influences, int mode) {
    const uint8_t* vertices = skin->vertices;
    const size_t stride = skin->vertex_stride;
    const int16_t (*positions)[POSITION_AXES] = bind->positions;
    const uint8_t (*colors)[R3D_SKIN_LIGHT_CHANNELS] = bind->colors;
    int16_t (*posed)[POSITION_AXES] = out->positions;
    for (int c = first; c < first + count; c++) {
        r3d_lit_cluster_t* cluster = &out->clusters[c];
        const int end = cluster->vertex_first + cluster->vertex_count;
        for (int v = cluster->vertex_first; v < end; v++) {
            vec3f_t normal;
            skin_lit_vertex(vertices + ((size_t)v * stride), influences, palette, positions[v], posed[v], normals,
                            &normal);
            skin_light_vertex(normal, lights, colors[v], light[v], mode);
        }
        cluster->bounds = cluster_bounds(posed, cluster);
    }
}

static void
skin_two(const r3d_skin_t* skin, const r3d_lit_mesh_t* bind, const mat4f_t* palette, int first, int count,
         r3d_skin_output_t* out) {
    skin_range(skin, bind, palette, first, count, out, R3D_SKIN_INFLUENCES_MIN);
}

static void
skin_four(const r3d_skin_t* skin, const r3d_lit_mesh_t* bind, const mat4f_t* palette, int first, int count,
          r3d_skin_output_t* out) {
    skin_range(skin, bind, palette, first, count, out, R3D_SKIN_INFLUENCES_MAX);
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

#define SKIN_LIGHT_KERNEL(name, mode)                                                                                  \
    void name(const r3d_skin_t* skin, const r3d_lit_mesh_t* bind, const mat4f_t* palette, int first, int count,        \
              r3d_skin_output_t* out, const mat3f_t* normals, const r3d_skin_lights_t* lights, uint8_t (*light)[3]) {  \
        assert(lights != NULL && normals != NULL);                                                                     \
        assert(lights->count >= 0 && lights->count <= R3D_SKIN_LIGHT_MAX);                                             \
        const r3d_skin_lights_t prepared = *lights;                                                                    \
        check_range(skin, bind, first, count);                                                                         \
        if (skin->influences == R3D_SKIN_INFLUENCES_MIN) {                                                             \
            skin_lit_range(skin, bind, palette, first, count, out, normals, &prepared, light, R3D_SKIN_INFLUENCES_MIN, \
                           mode);                                                                                      \
        } else {                                                                                                       \
            skin_lit_range(skin, bind, palette, first, count, out, normals, &prepared, light, R3D_SKIN_INFLUENCES_MAX, \
                           mode);                                                                                      \
        }                                                                                                              \
    }
SKIN_LIGHT_KERNEL(r3d_skin_clusters_lit, LIGHT_DIRECT)
SKIN_LIGHT_KERNEL(r3d_skin_clusters_renormalised, LIGHT_RENORMALISED)
SKIN_LIGHT_KERNEL(r3d_skin_clusters_table, LIGHT_TABLE)

void
r3d_skin_refit_nodes(r3d_skin_output_t* out) {
    r3d_lit_mesh_refit_nodes(out->nodes, out->mesh.node_count, out->clusters);
}
