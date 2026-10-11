/* Shared normal rotation, Lambert lighting and octahedral table kernels. */
#include "render/skin/r3d_skin_light.h"

#include <assert.h>
#include "math/scalar/mathf.h"

vec3f_t
r3d_skin_light_normal(const mat4f_t* palette, const uint8_t* joints, const float* weights, int influences,
                      vec3f_t normal) {
    assert(influences > 0 && influences <= R3D_SKIN_INFLUENCES_MAX);
    vec3f_t rows[R3D_SKIN_NORMAL_WIDTH];
    for (int axis = 0; axis < R3D_SKIN_NORMAL_WIDTH; axis++) {
        const float* r = palette[joints[0]].m[axis];
        rows[axis] = vec3f_scale((vec3f_t){r[0], r[1], r[2]}, weights[0]);
        for (int i = 1; i < influences; i++) {
            r = palette[joints[i]].m[axis];
            rows[axis] = vec3f_add(rows[axis], vec3f_scale((vec3f_t){r[0], r[1], r[2]}, weights[i]));
        }
    }
    return (vec3f_t){vec3f_dot(rows[0], normal), vec3f_dot(rows[1], normal), vec3f_dot(rows[2], normal)};
}

static uint8_t
light_byte(float value) {
    value = value * R3D_SKIN_LIGHT_BYTE_SCALE + 0.5F;
    return (uint8_t)fminf(R3D_SKIN_LIGHT_BYTE_SCALE, fmaxf(0.0F, value));
}

void
r3d_skin_light_evaluate(vec3f_t normal, const r3d_skin_lights_t* lights, uint8_t out[3]) {
    assert(lights->count >= 0 && lights->count <= R3D_SKIN_LIGHT_MAX);
    vec3f_t sum = lights->ambient;
    for (int i = 0; i < lights->count; i++) {
        const float d = vec3f_dot(normal, lights->direction[i]);
        if (d > 0.0F) {
            sum = vec3f_add(sum, vec3f_scale(lights->colour[i], d));
        }
    }
    out[0] = light_byte(sum.x);
    out[1] = light_byte(sum.y);
    out[2] = light_byte(sum.z);
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
    for (int c = 0; c < size * size; c++) {
        r3d_skin_light_evaluate(directions[c], lights, rgb[c]);
        rgb[c][R3D_SKIN_LIGHT_CHANNELS] = 0;
    }
}

static int
axis_cell(int size, float u, unsigned* weight) {
    const float centres = 0.5F * (float)size;
    const float x = fminf((float)(size - 1), fmaxf(0.0F, u * centres + centres - 0.5F));
    const int cell = (int)x < size - 2 ? (int)x : size - 2;
    *weight = (unsigned)((x - (float)cell) * R3D_SKIN_LIGHT_WEIGHT_ONE);
    return cell;
}

void
r3d_skin_light_bilinear(int size, const uint8_t (*rgb)[R3D_SKIN_LIGHT_CELL_BYTES], vec3f_t normal, uint8_t out[3]) {
    assert(size >= 2);
    const vec2f_t p = vec3f_octahedral(normal);
    unsigned wx, wy;
    const int x = axis_cell(size, p.x, &wx);
    const int y = axis_cell(size, p.y, &wy);
    const uint8_t* a = rgb[y * size + x];
    const uint8_t* b = rgb[y * size + x + 1];
    const uint8_t* c = rgb[(y + 1) * size + x];
    const uint8_t* d = rgb[(y + 1) * size + x + 1];
    const unsigned rounding = 1U << (2 * R3D_SKIN_LIGHT_WEIGHT_BITS - 1);
    for (int k = 0; k < R3D_SKIN_LIGHT_CHANNELS; k++) {
        const unsigned top = a[k] * (R3D_SKIN_LIGHT_WEIGHT_ONE - wx) + b[k] * wx;
        const unsigned bottom = c[k] * (R3D_SKIN_LIGHT_WEIGHT_ONE - wx) + d[k] * wx;
        out[k] = (uint8_t)((top * (R3D_SKIN_LIGHT_WEIGHT_ONE - wy) + bottom * wy + rounding)
                           >> (2 * R3D_SKIN_LIGHT_WEIGHT_BITS));
    }
}

void
r3d_skin_light_build(const vec3f_t* directions, const r3d_skin_lights_t* lights, r3d_skin_light_table_t* table) {
    r3d_skin_light_grid_build(R3D_SKIN_LIGHT_TABLE_SIZE, directions, lights, table->rgb);
}

static vec3f_t
record_normal(const r3d_skin_t* skin, const mat4f_t* palette, int vertex) {
    const uint8_t* record = skin->vertices + (size_t)vertex * skin->vertex_stride;
    const int8_t* n = (const int8_t*)(record + 2 * skin->influences);
    float weights[R3D_SKIN_INFLUENCES_MAX];
    for (int i = 0; i < skin->influences; i++) {
        weights[i] = (float)record[skin->influences + i] / R3D_SKIN_WEIGHT_SUM;
    }
    const vec3f_t normal = {(float)n[0] / R3D_SKIN_NORMAL_SCALE, (float)n[1] / R3D_SKIN_NORMAL_SCALE,
                            (float)n[2] / R3D_SKIN_NORMAL_SCALE};
    return r3d_skin_light_normal(palette, record, weights, skin->influences, normal);
}

static void
check_range(const r3d_skin_t* skin, const r3d_lit_mesh_t* bind, int first, int count) {
    assert(skin->vertex_count == (uint32_t)bind->vertex_count);
    assert(first >= 0 && count >= 0 && first <= bind->cluster_count && count <= bind->cluster_count - first);
    assert(skin->influences == R3D_SKIN_INFLUENCES_MIN || skin->influences == R3D_SKIN_INFLUENCES_MAX);
}

static void
light_range(const r3d_skin_t* skin, const r3d_lit_mesh_t* bind, const mat4f_t* palette, int first, int count,
            const r3d_skin_lights_t* lights, bool renormalise, const r3d_skin_light_table_t* table, uint8_t (*out)[3]) {
    check_range(skin, bind, first, count);
    for (int c = first; c < first + count; c++) {
        const r3d_lit_cluster_t* cluster = &bind->clusters[c];
        for (int v = cluster->vertex_first; v < cluster->vertex_first + cluster->vertex_count; v++) {
            vec3f_t n = record_normal(skin, palette, v);
            if (table != NULL) {
                r3d_skin_light_bilinear(R3D_SKIN_LIGHT_TABLE_SIZE, table->rgb, n, out[v]);
            } else {
                if (renormalise) {
                    n = vec3f_normalize(n);
                }
                r3d_skin_light_evaluate(n, lights, out[v]);
            }
        }
    }
}

void
r3d_skin_light_direct(const r3d_skin_t* skin, const r3d_lit_mesh_t* bind, const mat4f_t* palette, int first, int count,
                      const r3d_skin_lights_t* lights, bool renormalise, uint8_t (*out)[3]) {
    light_range(skin, bind, palette, first, count, lights, renormalise, NULL, out);
}

void
r3d_skin_light_table(const r3d_skin_t* skin, const r3d_lit_mesh_t* bind, const mat4f_t* palette, int first, int count,
                     const r3d_skin_light_table_t* table, uint8_t (*out)[3]) {
    assert(table != NULL);
    light_range(skin, bind, palette, first, count, NULL, false, table, out);
}
