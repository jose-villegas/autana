/* Shared normal rotation, Lambert lighting and octahedral table kernels. */
#include "render/skin/r3d_skin_light.h"

#include <assert.h>
#include "render/skin/r3d_skin_light_internal.h"

vec3f_t
r3d_skin_light_normal(const mat4f_t* palette, const uint8_t* joints, const float* weights, int influences,
                      vec3f_t normal) {
    assert(influences > 0 && influences <= R3D_SKIN_INFLUENCES_MAX);
    return light_normal(palette, joints, weights, influences, normal);
}

void
r3d_skin_light_evaluate(vec3f_t normal, const r3d_skin_lights_t* lights, uint8_t out[3]) {
    assert(lights->count >= 0 && lights->count <= R3D_SKIN_LIGHT_MAX);
    light_evaluate(normal, lights, out);
}

/* Octahedral directions: docs/Citations.md#21. Normal-indexed lighting: docs/Citations.md#58. */
void
r3d_skin_light_directions(int size, vec3f_t* directions) {
    assert(size >= 2);
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            const vec2f_t p = {((float)x + 0.5F) * 2.0F / (float)size - 1.0F,
                               ((float)y + 0.5F) * 2.0F / (float)size - 1.0F};
            directions[y * size + x] = vec3f_from_octahedral(p);
        }
    }
}

void
r3d_skin_light_grid_build(int size, const vec3f_t* directions, const r3d_skin_lights_t* lights,
                          uint8_t (*rgb)[R3D_SKIN_LIGHT_CELL_BYTES]) {
    assert(lights->count >= 0 && lights->count <= R3D_SKIN_LIGHT_MAX);
    for (int c = 0; c < size * size; c++) {
        light_evaluate(directions[c], lights, rgb[c]);
        rgb[c][R3D_SKIN_LIGHT_CHANNELS] = 0;
    }
}

void
r3d_skin_light_bilinear(int size, const uint8_t (*rgb)[R3D_SKIN_LIGHT_CELL_BYTES], vec3f_t normal, uint8_t out[3]) {
    assert(size >= 2);
    light_bilinear(size, rgb, normal, out);
}

void
r3d_skin_light_build(const vec3f_t* directions, const r3d_skin_lights_t* lights, r3d_skin_light_table_t* table) {
    r3d_skin_light_grid_build(R3D_SKIN_LIGHT_TABLE_SIZE, directions, lights, table->rgb);
}

static inline __attribute__((always_inline)) vec3f_t
record_normal(const r3d_skin_t* skin, const mat4f_t* palette, int vertex, int influences) {
    const uint8_t* record = skin->vertices + (size_t)vertex * skin->vertex_stride;
    const int8_t* n = (const int8_t*)(record + 2 * influences);
    float weights[R3D_SKIN_INFLUENCES_MAX];
    for (int i = 0; i < influences; i++) {
        weights[i] = (float)record[influences + i] * R3D_SKIN_WEIGHT_RECIP;
    }
    const vec3f_t normal = {(float)n[0] * R3D_SKIN_NORMAL_RECIP, (float)n[1] * R3D_SKIN_NORMAL_RECIP,
                            (float)n[2] * R3D_SKIN_NORMAL_RECIP};
    return light_normal(palette, record, weights, influences, normal);
}

static inline __attribute__((always_inline)) void
light_range(const r3d_skin_t* skin, const r3d_lit_mesh_t* bind, const mat4f_t* palette, int first, int count,
            const r3d_skin_lights_t* lights, bool renormalise, const r3d_skin_light_table_t* table, uint8_t (*out)[3],
            int influences) {
    check_range(skin, bind, first, count);
    assert(table != NULL || (lights->count >= 0 && lights->count <= R3D_SKIN_LIGHT_MAX));
    for (int c = first; c < first + count; c++) {
        const r3d_lit_cluster_t* cluster = &bind->clusters[c];
        for (int v = cluster->vertex_first; v < cluster->vertex_first + cluster->vertex_count; v++) {
            vec3f_t n = record_normal(skin, palette, v, influences);
            if (table != NULL) {
                light_bilinear(R3D_SKIN_LIGHT_TABLE_SIZE, table->rgb, n, out[v]);
            } else {
                if (renormalise) {
                    n = light_renormalise(n);
                }
                light_evaluate(n, lights, out[v]);
            }
        }
    }
}

#define LIGHT_RANGE(name, influences, lookup)                                                                          \
    static void name(const r3d_skin_t* skin, const r3d_lit_mesh_t* bind, const mat4f_t* palette, int first, int count, \
                     const r3d_skin_lights_t* lights, bool renormalise, const r3d_skin_light_table_t* table,           \
                     uint8_t(*out)[3]) {                                                                               \
        light_range(skin, bind, palette, first, count, lights, renormalise, lookup ? table : NULL, out, influences);   \
    }
LIGHT_RANGE(direct_two, R3D_SKIN_INFLUENCES_MIN, false)
LIGHT_RANGE(direct_four, R3D_SKIN_INFLUENCES_MAX, false)
LIGHT_RANGE(table_two, R3D_SKIN_INFLUENCES_MIN, true)
LIGHT_RANGE(table_four, R3D_SKIN_INFLUENCES_MAX, true)

void
r3d_skin_light_direct(const r3d_skin_t* skin, const r3d_lit_mesh_t* bind, const mat4f_t* palette, int first, int count,
                      const r3d_skin_lights_t* lights, bool renormalise, uint8_t (*out)[3]) {
    if (skin->influences == R3D_SKIN_INFLUENCES_MIN) {
        direct_two(skin, bind, palette, first, count, lights, renormalise, NULL, out);
    } else {
        direct_four(skin, bind, palette, first, count, lights, renormalise, NULL, out);
    }
}

void
r3d_skin_light_table(const r3d_skin_t* skin, const r3d_lit_mesh_t* bind, const mat4f_t* palette, int first, int count,
                     const r3d_skin_light_table_t* table, uint8_t (*out)[3]) {
    assert(table != NULL);
    if (skin->influences == R3D_SKIN_INFLUENCES_MIN) {
        table_two(skin, bind, palette, first, count, NULL, false, table, out);
    } else {
        table_four(skin, bind, palette, first, count, NULL, false, table, out);
    }
}
