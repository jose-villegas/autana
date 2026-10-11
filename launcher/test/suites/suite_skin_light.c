/* Lighting contracts cover transfer accuracy and split cluster ranges. */
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "math/linear/transformf.h"
#include "render/skin/r3d_skin_light_internal.h"
#include "skin_light_reference.h"
#include "suites.h"
#include "unity.h"

enum { VERTICES = 4, CLUSTERS = 2, JOINTS = 4, SENTINEL = 19 };

static const r3d_lit_cluster_t CLUSTER_ROWS[] = {{.vertex_first = 0, .vertex_count = 2},
                                                 {.vertex_first = 2, .vertex_count = 2}};

typedef struct {
    r3d_skin_t skin;
    r3d_lit_mesh_t bind;
    mat4f_t palette[JOINTS];
    mat3f_t normals[JOINTS];
    uint8_t records[VERTICES * (2 * R3D_SKIN_INFLUENCES_MAX + R3D_SKIN_RECORD_TAIL)];
    uint8_t out[VERTICES][3];
} fixture_t;

static void
fixture(int influences, fixture_t* f) {
    *f = (fixture_t){.bind = {.vertex_count = VERTICES, .cluster_count = CLUSTERS, .clusters = CLUSTER_ROWS},
                     .skin = {.joint_count = JOINTS,
                              .influences = influences,
                              .vertex_count = VERTICES,
                              .vertex_stride = 2 * influences + R3D_SKIN_RECORD_TAIL}};
    for (int j = 0; j < JOINTS; j++) {
        f->palette[j] = mat4f_identity();
        f->normals[j] = mat3f_from_mat4(f->palette[j]);
    }
    for (int v = 0; v < VERTICES; v++) {
        uint8_t* r = f->records + v * f->skin.vertex_stride;
        for (int j = 0; j < influences; j++) {
            r[j] = j;
            r[influences + j] = R3D_SKIN_WEIGHT_SUM / influences;
        }
        r[influences] += R3D_SKIN_WEIGHT_SUM % influences;
        r[2 * influences + v % R3D_SKIN_NORMAL_WIDTH] = R3D_SKIN_NORMAL_SCALE;
    }
    memset(f->out, SENTINEL, sizeof f->out);
}

static void*
fixture_output(const fixture_t* f, r3d_skin_output_t* out) {
    const size_t bytes = r3d_skin_output_bytes(&f->bind);
    void* block = malloc(bytes);
    TEST_ASSERT_NOT_NULL(block);
    TEST_ASSERT_TRUE(r3d_skin_output_init(&f->bind, block, bytes, out));
    return block;
}

static void
fixture_light(fixture_t* f, r3d_skin_light_kernel_t kernel) {
    r3d_skin_light_transfer_t* transfer = malloc(sizeof *transfer);
    TEST_ASSERT_NOT_NULL(transfer);
    r3d_skin_light_transfer_build(0, transfer);
    const r3d_skin_lights_t lights = {.count = 1, .direction = {{0, 1, 0}}, .color = {{1, 1, 1}}, .transfer = transfer};
    r3d_skin_output_t output;
    void* block = fixture_output(f, &output);
    kernel(&f->skin, &f->bind, f->palette, 0, CLUSTERS, &output, f->normals, &lights, f->out);
    free(block);
    free(transfer);
}

static void
test_reference_decodes_packed_normal_length(void) {
    fixture_t* f = malloc(sizeof *f);
    TEST_ASSERT_NOT_NULL(f);
    for (int influences = R3D_SKIN_INFLUENCES_MIN; influences <= R3D_SKIN_INFLUENCES_MAX; influences += 2) {
        fixture(influences, f);
        f->skin.vertices = f->records;
        for (int v = 0; v < VERTICES; v++) {
            int8_t* bind = (int8_t*)(f->records + v * f->skin.vertex_stride + 2 * influences);
            bind[0] = -R3D_SKIN_NORMAL_SCALE;
            bind[1] = R3D_SKIN_NORMAL_SCALE / 2;
            bind[2] = R3D_SKIN_NORMAL_SCALE;
            const vec3f_t got = skin_light_reference_normal(&f->skin, f->normals, v, false);
            TEST_ASSERT_FLOAT_WITHIN(0.00001F, -1.0F, got.x);
            TEST_ASSERT_FLOAT_WITHIN(0.00001F, bind[1] * R3D_SKIN_NORMAL_RECIP, got.y);
            TEST_ASSERT_FLOAT_WITHIN(0.00001F, 1.0F, got.z);
        }
    }
    free(f);
}

static void
test_transfer_error_against_float_bake(void) {
    static const float whites[] = {0, 0.25F, 0.5F, 1, 2, 4, 16};

    enum { SAMPLES_PER_BIN = 16 };

    r3d_skin_light_transfer_t* transfer = malloc(sizeof *transfer);
    TEST_ASSERT_NOT_NULL(transfer);
    _Static_assert(sizeof *transfer <= 1024, "transfer budget");
    for (size_t w = 0; w < sizeof whites / sizeof whites[0]; w++) {
        r3d_skin_light_transfer_build(whites[w], transfer);
        for (int i = 0;
             i < ((R3D_SKIN_LIGHT_RADIANCE_MAX_EXP - R3D_SKIN_LIGHT_RADIANCE_MIN_EXP) << R3D_SKIN_LIGHT_RADIANCE_BITS)
                     * SAMPLES_PER_BIN;
             i++) {
            const float linear = exp2f(R3D_SKIN_LIGHT_RADIANCE_MIN_EXP
                                       + (float)i / (SAMPLES_PER_BIN * (1 << R3D_SKIN_LIGHT_RADIANCE_BITS)));
            TEST_ASSERT_UINT8_WITHIN(R3D_SKIN_LIGHT_TRANSFER_ERROR_BOUND,
                                     skin_light_reference_encode(linear, whites[w]), light_encode(linear, transfer));
        }
        for (int albedo = 0; albedo < R3D_SKIN_LIGHT_DECODE_SIZE; albedo++) {
            const float exact = powf((float)albedo / R3D_SKIN_LIGHT_BYTE_SCALE, R3D_SKIN_LIGHT_GAMMA);
            const float decoded = light_decode((uint8_t)albedo, transfer);
            for (int intensity = 0; intensity <= 64; intensity++) {
                const float value = intensity * 0.25F;
                TEST_ASSERT_UINT8_WITHIN(R3D_SKIN_LIGHT_TRANSFER_ERROR_BOUND,
                                         skin_light_reference_encode(exact * value, whites[w]),
                                         light_encode(decoded * value, transfer));
            }
        }
        TEST_ASSERT_EQUAL_UINT8(0, light_encode(0, transfer));
        TEST_ASSERT_UINT8_WITHIN(1, skin_light_reference_encode(4096, whites[w]), light_encode(4096, transfer));
    }
    free(transfer);
}

/* Host fallback checks wiring only; the device sweep checks the FPU instruction. */
static void
test_rsqrt_dense_normals_match_exact_light_bytes(void) {
    enum { DIRECTIONS = 2048, LENGTHS = 32 };

    r3d_skin_light_transfer_t* transfer = malloc(sizeof *transfer);
    TEST_ASSERT_NOT_NULL(transfer);
    r3d_skin_light_transfer_build(1.0F, transfer);
    const r3d_skin_lights_t lights = {.count = 1, .direction = {{0, 1, 0}}, .color = {{1, 1, 1}}, .transfer = transfer};
    const uint8_t albedo[] = {255, 128, 32};
    for (int i = 0; i < DIRECTIONS; i++) {
        const float z = -1.0F + 2.0F * (i + 0.5F) / DIRECTIONS;
        const float angle = i * 2.39996323F;
        const float radius = sqrtf(1.0F - z * z);
        const vec3f_t unit = {radius * cosf(angle), radius * sinf(angle), z};
        for (int length = 1; length <= LENGTHS; length++) {
            const vec3f_t n = vec3f_scale(unit, exp2f((float)length - LENGTHS / 2));
            const vec3f_t exact = vec3f_scale(n, 1.0F / sqrtf(vec3f_dot(n, n)));
            const float squared = vec3f_dot(n, n);
            const float exact_recip = 1.0F / sqrtf(squared);
            TEST_ASSERT_FLOAT_WITHIN(exact_recip * MATHF_RSQRT_RELATIVE_LIMIT, exact_recip, mathf_rsqrt(squared));
            const vec3f_t fast = light_renormalise(n);
            uint8_t want[3], got[3];
            light_evaluate(exact, &lights, albedo, want);
            light_evaluate(fast, &lights, albedo, got);
            for (int k = 0; k < R3D_SKIN_LIGHT_CHANNELS; k++) {
                TEST_ASSERT_UINT8_WITHIN(1, want[k], got[k]);
            }
        }
    }
    free(transfer);
}

static void
test_fused_decodes_albedo_and_split_matches_single(void) {
    static const int16_t positions[VERTICES][3] = {{-23, 51, 7}, {91, -31, 16}, {7, 19, -52}, {-41, 13, 37}};
    static const uint8_t colors[VERTICES][3] = {{128, 64, 32}, {255, 19, 83}, {47, 211, 128}, {91, 173, 237}};
    for (int influences = R3D_SKIN_INFLUENCES_MIN; influences <= R3D_SKIN_INFLUENCES_MAX; influences += 2) {
        fixture_t* f = malloc(sizeof *f);
        TEST_ASSERT_NOT_NULL(f);
        fixture(influences, f);
        f->skin.vertices = f->records;
        f->bind.positions = positions;
        f->bind.colors = colors;
        r3d_skin_light_transfer_t* transfer = malloc(sizeof *transfer);
        TEST_ASSERT_NOT_NULL(transfer);
        r3d_skin_light_transfer_build(0, transfer);
        const r3d_skin_lights_t lights = {.ambient = {0.5F, 0.5F, 0.5F}, .transfer = transfer};
        const size_t bytes = r3d_skin_output_bytes(&f->bind);
        void* blocks[2] = {malloc(bytes), malloc(bytes)};
        TEST_ASSERT_NOT_NULL(blocks[0]);
        TEST_ASSERT_NOT_NULL(blocks[1]);
        r3d_skin_output_t single, split;
        TEST_ASSERT_TRUE(r3d_skin_output_init(&f->bind, blocks[0], bytes, &single));
        TEST_ASSERT_TRUE(r3d_skin_output_init(&f->bind, blocks[1], bytes, &split));
        uint8_t split_colors[VERTICES][3];
        r3d_skin_clusters_renormalised(&f->skin, &f->bind, f->palette, 0, CLUSTERS, &single, f->normals, &lights,
                                       f->out);
        for (int v = 0; v < VERTICES; v++) {
            for (int k = 0; k < R3D_SKIN_LIGHT_CHANNELS; k++) {
                const float decoded = powf((float)colors[v][k] / R3D_SKIN_LIGHT_BYTE_SCALE, R3D_SKIN_LIGHT_GAMMA);
                TEST_ASSERT_UINT8_WITHIN(2, skin_light_reference_encode(decoded * lights.ambient.x, 0), f->out[v][k]);
            }
        }
        for (int c = CLUSTERS - 1; c >= 0; c--) {
            r3d_skin_clusters_renormalised(&f->skin, &f->bind, f->palette, c, 1, &split, f->normals, &lights,
                                           split_colors);
        }
        TEST_ASSERT_EQUAL_MEMORY(f->out, split_colors, sizeof f->out);
        TEST_ASSERT_EQUAL_MEMORY(single.positions, split.positions, sizeof positions);
        TEST_ASSERT_EQUAL_MEMORY(single.clusters, split.clusters, sizeof CLUSTER_ROWS);
        r3d_skin_clusters_renormalised(&f->skin, &f->bind, f->palette, CLUSTERS, 0, &split, f->normals, &lights,
                                       split_colors);
        TEST_ASSERT_EQUAL_MEMORY(f->out, split_colors, sizeof f->out);
        free(blocks[1]);
        free(blocks[0]);
        free(transfer);
        free(f);
    }
}

static void
test_direct_sum_matches_independent_float_reference(void) {
    static const vec3f_t directions[] = {{0, 1, 0}, {0, -1, 0}, {1, 0, 0}, {0, 1, 0}};
    static const vec3f_t colors[] = {{0.2F, 0.3F, 0.4F}, {1, 1, 1}, {1, 1, 1}, {0.4F, 0.1F, 0.2F}};
    static const uint8_t albedo[] = {91, 173, 237};
    r3d_skin_light_transfer_t* transfer = malloc(sizeof *transfer);
    TEST_ASSERT_NOT_NULL(transfer);
    r3d_skin_light_transfer_build(1, transfer);
    for (int count = 2; count <= R3D_SKIN_LIGHT_MAX; count += 2) {
        r3d_skin_lights_t lights = {.count = count, .ambient = {0.1F, 0.2F, 0.3F}, .transfer = transfer};
        memcpy(lights.direction, directions, sizeof directions);
        memcpy(lights.color, colors, sizeof colors);
        uint8_t got[3];
        light_evaluate((vec3f_t){0, 1, 0}, &lights, albedo, got);
        const float sum[] = {
            lights.ambient.x + lights.color[0].x + (count == R3D_SKIN_LIGHT_MAX ? lights.color[3].x : 0),
            lights.ambient.y + lights.color[0].y + (count == R3D_SKIN_LIGHT_MAX ? lights.color[3].y : 0),
            lights.ambient.z + lights.color[0].z + (count == R3D_SKIN_LIGHT_MAX ? lights.color[3].z : 0)};
        for (int k = 0; k < R3D_SKIN_LIGHT_CHANNELS; k++) {
            const float decoded = powf((float)albedo[k] / R3D_SKIN_LIGHT_BYTE_SCALE, R3D_SKIN_LIGHT_GAMMA);
            TEST_ASSERT_UINT8_WITHIN(2, skin_light_reference_encode(decoded * sum[k], 1), got[k]);
        }
    }
    free(transfer);
}

static void
assert_rotated_reference(const fixture_t* f, const uint8_t colors[VERTICES][3], const r3d_skin_lights_t* lights,
                         bool normalise, int bound) {
    for (int v = 0; v < VERTICES; v++) {
        const vec3f_t normal = skin_light_reference_normal(&f->skin, f->normals, v, normalise);
        uint8_t expected[R3D_SKIN_LIGHT_CHANNELS];
        skin_light_reference(normal, lights, colors[v], 1, expected);
        for (int k = 0; k < R3D_SKIN_LIGHT_CHANNELS; k++) {
            TEST_ASSERT_UINT8_WITHIN(bound, expected[k], f->out[v][k]);
        }
    }
}

static void
test_fused_rotations_unequal_weights_match_independent_reference(void) {
    static const int16_t positions[VERTICES][3] = {{1, 2, 3}, {4, 5, 6}, {7, 8, 9}, {3, 2, 1}};
    static const uint8_t colors[VERTICES][3] = {{91, 173, 237}, {237, 91, 173}, {173, 237, 91}, {64, 128, 255}};
    static const uint8_t weights[][JOINTS] = {{173, 82, 0, 0}, {113, 71, 43, 28}};

    static const struct {
        r3d_skin_light_kernel_t kernel;
        bool normalise;
        int bound;
    } modes[] = {{r3d_skin_clusters_lit, false, R3D_SKIN_LIGHT_TRANSFER_ERROR_BOUND},
                 {r3d_skin_clusters_renormalised, true, R3D_SKIN_LIGHT_TRANSFER_ERROR_BOUND},
                 {r3d_skin_clusters_table, true, R3D_SKIN_LIGHT_TABLE_ERROR_BOUND}};

    for (int influences = R3D_SKIN_INFLUENCES_MIN; influences <= R3D_SKIN_INFLUENCES_MAX; influences += 2) {
        fixture_t* f = malloc(sizeof *f);
        TEST_ASSERT_NOT_NULL(f);
        fixture(influences, f);
        f->skin.vertices = f->records;
        f->bind.positions = positions;
        f->bind.colors = colors;
        for (int j = 0; j < JOINTS; j++) {
            transformf_t transform = {.rotation = quatf_identity(), .scale = {1, 1, 1}};
            transform.rotation = quatf_from_euler((vec3f_t){0.17F * (j + 1), -0.31F * j, 0.23F * (j + 1)});
            f->palette[j] = transformf_matrix(&transform);
            f->normals[j] = mat3f_cofactor(mat3f_from_mat4(f->palette[j]));
        }
        for (int v = 0; v < VERTICES; v++) {
            memcpy(f->records + v * f->skin.vertex_stride + influences, weights[influences / 2 - 1], influences);
        }
        r3d_skin_light_transfer_t* transfer = malloc(sizeof *transfer);
        TEST_ASSERT_NOT_NULL(transfer);
        r3d_skin_light_transfer_build(1, transfer);
        r3d_skin_lights_t lights = {.count = 1,
                                    .direction = {{0, 1, 0}},
                                    .color = {{1, 0.7F, 0.3F}},
                                    .ambient = {0.1F, 0.2F, 0.3F},
                                    .transfer = transfer};
        r3d_skin_output_t posed;
        void* block = fixture_output(f, &posed);
        r3d_skin_light_table_t* table = malloc(sizeof *table);
        TEST_ASSERT_NOT_NULL(table);
        lights.table = table;
        lights.table_scale = r3d_skin_light_table_build(&lights, table);
        for (size_t mode = 0; mode < sizeof modes / sizeof modes[0]; mode++) {
            modes[mode].kernel(&f->skin, &f->bind, f->palette, 0, CLUSTERS, &posed, f->normals, &lights, f->out);
            assert_rotated_reference(f, colors, &lights, modes[mode].normalise, modes[mode].bound);
        }
        free(table);
        free(block);
        free(transfer);
        free(f);
    }
}

static void
test_encode_invalid_and_range_edges(void) {
    r3d_skin_light_transfer_t* transfer = malloc(sizeof *transfer);
    TEST_ASSERT_NOT_NULL(transfer);
    r3d_skin_light_transfer_build(1, transfer);
    TEST_ASSERT_EQUAL_UINT8(0, light_encode(NAN, transfer));
    TEST_ASSERT_EQUAL_UINT8(0, light_encode(-1, transfer));
    TEST_ASSERT_EQUAL_UINT8(0, light_encode(exp2f(R3D_SKIN_LIGHT_RADIANCE_MIN_EXP - 1), transfer));
    TEST_ASSERT_EQUAL_UINT8(transfer->encode[R3D_SKIN_LIGHT_RADIANCE_SIZE - 1], light_encode(INFINITY, transfer));
    free(transfer);
}

static void
assert_scaled_lighting(bool joint_scale, bool rotate, bool distinct) {
    static const int16_t positions[VERTICES][3] = {{0}};
    static const uint8_t colors[VERTICES][3] = {{255, 255, 255}, {255, 255, 255}, {255, 255, 255}, {255, 255, 255}};
    fixture_t* f = malloc(sizeof *f);
    TEST_ASSERT_NOT_NULL(f);
    fixture(R3D_SKIN_INFLUENCES_MIN, f);
    f->skin.vertices = f->records;
    f->bind.positions = positions;
    f->bind.colors = colors;
    transformf_t entity = {.rotation = quatf_identity(), .scale = {1, 1, 1}};
    transformf_t joint = entity;
    if (joint_scale) {
        joint.scale.x = 2;
    } else {
        entity.scale.x = 2;
    }
    if (rotate) {
        joint.rotation = quatf_from_euler((vec3f_t){0.3F, -0.4F, 0.7F});
        entity.rotation = quatf_from_euler((vec3f_t){-0.2F, 0.5F, 0.1F});
        entity.scale = (vec3f_t){2, 3, 1};
    }
    for (int j = 0; j < JOINTS; j++) {
        f->palette[j] = distinct && j == 0 ? mat4f_identity() : transformf_matrix(&joint);
    }
    for (int v = 0; v < VERTICES; v++) {
        int8_t* n = (int8_t*)(f->records + v * f->skin.vertex_stride + 2 * f->skin.influences);
        n[0] = n[1] = R3D_SKIN_NORMAL_SCALE;
        n[2] = 0;
    }
    r3d_skin_normal_palette(f->palette, JOINTS, transformf_matrix(&entity), f->normals);
    /* TRS inverse-transpose independently applies reciprocal scale then quaternion rotation. */
    vec3f_t expected = quatf_rotate(joint.rotation, (vec3f_t){1 / joint.scale.x, 1 / joint.scale.y, 0});
    expected = quatf_rotate(entity.rotation, (vec3f_t){expected.x / entity.scale.x, expected.y / entity.scale.y,
                                                       expected.z / entity.scale.z});
    if (distinct) {
        const float first = (float)(R3D_SKIN_WEIGHT_SUM / 2 + 1) * R3D_SKIN_WEIGHT_RECIP;
        expected = (vec3f_t){first + (1 - first) / joint.scale.x, 1, 0};
    }
    const float facing = fmaxf(0, expected.y / sqrtf(vec3f_dot(expected, expected)));
    fixture_light(f, r3d_skin_clusters_renormalised);
    for (int v = 0; v < VERTICES; v++) {
        TEST_ASSERT_UINT8_WITHIN(2, skin_light_reference_encode(facing, 0), f->out[v][0]);
    }
    free(f);
}

static void
test_joint_nonuniform_scale(void) {
    assert_scaled_lighting(true, false, false);
}

static void
test_entity_nonuniform_scale(void) {
    assert_scaled_lighting(false, false, false);
}

static void
test_joint_entity_rotation_and_scale(void) {
    assert_scaled_lighting(true, true, false);
}

static void
test_distinct_joint_determinants_preserve_weights(void) {
    assert_scaled_lighting(true, false, true);
}

static void
test_normal_palette_refuses_nonfinite_inverse(void) {
    const transformf_t transform = {.rotation = {0, 0, 0, 1}, .scale = {1e-39F, 1e19F, 1e19F}};
    const mat4f_t palette = transformf_compute_matrix(&transform);
    mat3f_t normal;
    TEST_ASSERT_FALSE(r3d_skin_normal_palette(&palette, 1, mat4f_identity(), &normal));
}

static void
test_default_direct_preserves_blended_normal_length(void) {
    fixture_t* f = malloc(sizeof *f);
    TEST_ASSERT_NOT_NULL(f);
    fixture(R3D_SKIN_INFLUENCES_MIN, f);
    f->skin.vertices = f->records;
    static const int16_t positions[VERTICES][3] = {{0}};
    static const uint8_t colors[VERTICES][3] = {{255, 255, 255}, {255, 255, 255}, {255, 255, 255}, {255, 255, 255}};
    f->bind.positions = positions;
    f->bind.colors = colors;
    for (int j = 0; j < JOINTS; j++) {
        f->normals[j].m[1][1] = 0.25F;
    }
    fixture_light(f, r3d_skin_clusters_lit);
    TEST_ASSERT_UINT8_WITHIN(2, skin_light_reference_encode(0.25F, 0), f->out[1][0]);
    free(f);
}

static int
assert_table_sample(vec3f_t normal, const r3d_skin_lights_t* lights, float white) {
    static const uint8_t albedo[] = {91, 173, 237};
    uint8_t direct[3], sampled[3], expected[3];
    skin_light_reference(normal, lights, albedo, white, expected);
    light_evaluate(normal, lights, albedo, direct);
    light_shade(r3d_skin_light_table_sample(vec3f_scale(normal, 0.25F), lights), lights, albedo, sampled);
    int maximum = 0;
    for (int k = 0; k < R3D_SKIN_LIGHT_CHANNELS; k++) {
        TEST_ASSERT_UINT8_WITHIN(R3D_SKIN_LIGHT_TRANSFER_ERROR_BOUND, expected[k], direct[k]);
        TEST_ASSERT_UINT8_WITHIN(R3D_SKIN_LIGHT_TABLE_ERROR_BOUND, expected[k], sampled[k]);
        const int error = abs((int)expected[k] - sampled[k]);
        maximum = error > maximum ? error : maximum;
    }
    return maximum;
}

static int
assert_table_sweep(const r3d_skin_lights_t* lights, float white) {
    enum { GRID = 31 };

    int maximum = 0;
    for (int y = -GRID; y <= GRID; y++) {
        for (int x = -GRID; x <= GRID; x++) {
            const vec3f_t normal = vec3f_from_octahedral((vec2f_t){(float)x / GRID, (float)y / GRID});
            const int error = assert_table_sample(normal, lights, white);
            maximum = error > maximum ? error : maximum;
        }
    }
    return maximum;
}

static void
test_irradiance_table_preserves_range_and_matches_float(void) {
    TEST_ASSERT_EQUAL_INT(CHAR_BIT, R3D_SKIN_LIGHT_TABLE_BITS);
    int maximum = 0;

    r3d_skin_light_transfer_t* transfer = malloc(sizeof *transfer);
    r3d_skin_light_table_t* table = malloc(sizeof *table);
    TEST_ASSERT_NOT_NULL(transfer);
    TEST_ASSERT_NOT_NULL(table);
    _Static_assert(sizeof *table == R3D_SKIN_LIGHT_TABLE_CELLS * sizeof(uint32_t), "table cell budget");
    static const vec3f_t directions[] = {{0, 1, 0}, {0, 0, 1}, {1, 0, 0}, {0, -1, 0}};
    for (int count = 1; count <= R3D_SKIN_LIGHT_MAX; count *= 2) {
        r3d_skin_lights_t lights = {
            .count = count, .ambient = {0.15F, 0.2F, 0.25F}, .transfer = transfer, .table = table};
        memcpy(lights.direction, directions, sizeof directions);
        for (int i = 0; i < count; i++) {
            lights.color[i] = (vec3f_t){3, 2, 4};
        }
        lights.table_scale = r3d_skin_light_table_build(&lights, table);
        TEST_ASSERT_GREATER_THAN_FLOAT(1, r3d_skin_light_table_sample((vec3f_t){0, 1, 0}, &lights).z);
        for (int white = 0; white <= 1; white++) {
            r3d_skin_light_transfer_build((float)white, transfer);
            const int error = assert_table_sweep(&lights, (float)white);
            maximum = error > maximum ? error : maximum;
        }
    }
    TEST_ASSERT_LESS_OR_EQUAL_INT(R3D_SKIN_LIGHT_TABLE_ERROR_BOUND, maximum);
    const vec3f_t zero =
        r3d_skin_light_table_sample((vec3f_t){0}, &(r3d_skin_lights_t){.table = table, .ambient = {1, 2, 3}});
    TEST_ASSERT_EQUAL_FLOAT(2, zero.y);
    free(table);
    free(transfer);
}

static float
assert_table_cell(const r3d_skin_lights_t* lights, int x, int y) {
    float maximum = 0.0F;
    const vec2f_t point = {2.0F * x / (R3D_SKIN_LIGHT_TABLE_SIDE - 1) - 1.0F,
                           2.0F * y / (R3D_SKIN_LIGHT_TABLE_SIDE - 1) - 1.0F};
    const vec3f_t direct = light_irradiance(vec3f_from_octahedral(point), lights);
    const float channels[] = {direct.x, direct.y, direct.z};
    const uint32_t packed = lights->table->cells[y * R3D_SKIN_LIGHT_TABLE_SIDE + x];
    TEST_ASSERT_EQUAL_UINT32(0, packed >> (R3D_SKIN_LIGHT_CHANNELS * R3D_SKIN_LIGHT_TABLE_BITS));
    for (int channel = 0; channel < R3D_SKIN_LIGHT_CHANNELS; channel++) {
        const uint32_t quantized = (packed >> (channel * R3D_SKIN_LIGHT_TABLE_BITS)) & R3D_SKIN_LIGHT_TABLE_MASK;
        TEST_ASSERT_FLOAT_WITHIN(lights->table_scale * 0.5001F, channels[channel], quantized * lights->table_scale);
        if (channels[channel] == 0) {
            TEST_ASSERT_EQUAL_UINT32(0, quantized);
        }
        maximum = fmaxf(maximum, channels[channel]);
    }
    return maximum;
}

static void
test_table_scale_tracks_sampled_maximum_and_cells(void) {
    r3d_skin_light_table_t* table = malloc(sizeof *table);
    TEST_ASSERT_NOT_NULL(table);
    static const vec3f_t directions[] = {{0, 1, 0}, {0, -1, 0}, {1, 0, 0}, {-1, 0, 0}};
    const vec3f_t color = {1, 2, 4};
    for (int count = 0; count <= R3D_SKIN_LIGHT_MAX; count++) {
        r3d_skin_lights_t lights = {.count = count, .table = table};
        memcpy(lights.direction, directions, sizeof directions);
        for (int i = 0; i < count; i++) {
            lights.color[i] = color;
        }
        lights.table_scale = r3d_skin_light_table_build(&lights, table);
        float maximum = 0.0F;
        for (int y = 0; y < R3D_SKIN_LIGHT_TABLE_SIDE; y++) {
            for (int x = 0; x < R3D_SKIN_LIGHT_TABLE_SIDE; x++) {
                maximum = fmaxf(maximum, assert_table_cell(&lights, x, y));
            }
        }
        if (count == 0) {
            TEST_ASSERT_EQUAL_FLOAT(1.0F, lights.table_scale);
        } else {
            TEST_ASSERT_EQUAL_FLOAT(maximum, lights.table_scale * R3D_SKIN_LIGHT_TABLE_MASK);
        }
    }
    free(table);
}

static void
test_table_extreme_finite_intensities(void) {
    r3d_skin_light_table_t* table = malloc(sizeof *table);
    TEST_ASSERT_NOT_NULL(table);
    static const float intensities[] = {1e-38F, 3e38F};
    for (size_t i = 0; i < sizeof intensities / sizeof intensities[0]; i++) {
        r3d_skin_lights_t lights = {.count = 2, .table = table};
        for (int j = 0; j < lights.count; j++) {
            lights.direction[j] = (vec3f_t){0, 1, 0};
            lights.color[j] = (vec3f_t){intensities[i], intensities[i], intensities[i]};
        }
        lights.table_scale = r3d_skin_light_table_build(&lights, table);
        TEST_ASSERT_TRUE(isfinite(lights.table_scale));
        TEST_ASSERT_GREATER_THAN_FLOAT(0, lights.table_scale);
        const vec3f_t sampled = r3d_skin_light_table_sample((vec3f_t){0, 1, 0}, &lights);
        TEST_ASSERT_TRUE(isfinite(sampled.x));
        if (i == 0) {
            TEST_ASSERT_EQUAL_FLOAT(1, lights.table_scale);
            for (int cell = 0; cell < R3D_SKIN_LIGHT_TABLE_CELLS; cell++) {
                TEST_ASSERT_EQUAL_UINT32(0, table->cells[cell]);
            }
        } else {
            TEST_ASSERT_GREATER_THAN_FLOAT(1, sampled.x);
        }
    }
    free(table);
}

void
suite_skin_light(void) {
    RUN_TEST(test_reference_decodes_packed_normal_length);
    RUN_TEST(test_table_extreme_finite_intensities);
    RUN_TEST(test_table_scale_tracks_sampled_maximum_and_cells);
    RUN_TEST(test_irradiance_table_preserves_range_and_matches_float);
    RUN_TEST(test_default_direct_preserves_blended_normal_length);
    RUN_TEST(test_normal_palette_refuses_nonfinite_inverse);
    RUN_TEST(test_distinct_joint_determinants_preserve_weights);
    RUN_TEST(test_joint_nonuniform_scale);
    RUN_TEST(test_entity_nonuniform_scale);
    RUN_TEST(test_joint_entity_rotation_and_scale);
    RUN_TEST(test_direct_sum_matches_independent_float_reference);
    RUN_TEST(test_fused_rotations_unequal_weights_match_independent_reference);
    RUN_TEST(test_encode_invalid_and_range_edges);
    RUN_TEST(test_transfer_error_against_float_bake);
    RUN_TEST(test_rsqrt_dense_normals_match_exact_light_bytes);
    RUN_TEST(test_fused_decodes_albedo_and_split_matches_single);
}

SUITE_REGISTER(suite_skin_light);
