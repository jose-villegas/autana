/* Lighting contracts cover transfer accuracy and split cluster ranges. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "math/linear/transformf.h"
#include "render/skin/r3d_skin_light_internal.h"
#include "suites.h"
#include "unity.h"

enum { VERTICES = 4, CLUSTERS = 2, JOINTS = 4, SENTINEL = 19 };

static const r3d_lit_cluster_t CLUSTER_ROWS[] = {{.vertex_first = 0, .vertex_count = 2},
                                                 {.vertex_first = 2, .vertex_count = 2}};

typedef struct {
    r3d_skin_t skin;
    r3d_lit_mesh_t bind;
    mat4f_t palette[JOINTS];
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

static uint8_t
reference(float linear, float white) {
    const float mapped = fminf(linear / (1.0F + linear * white), 1.0F);
    return (uint8_t)lrintf(R3D_SKIN_LIGHT_BYTE_SCALE * powf(mapped, 1.0F / R3D_SKIN_LIGHT_GAMMA));
}

static void
test_transfer_error_against_float_bake(void) {
    static const float whites[] = {0, 0.25F, 0.5F, 1, 2, 4, 16};

    enum { SAMPLES_PER_BIN = 16, MAX_ERROR = 2 };

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
            TEST_ASSERT_UINT8_WITHIN(MAX_ERROR, reference(linear, whites[w]), light_encode(linear, transfer));
        }
        for (int albedo = 0; albedo < R3D_SKIN_LIGHT_DECODE_SIZE; albedo++) {
            const float exact = powf((float)albedo / R3D_SKIN_LIGHT_BYTE_SCALE, R3D_SKIN_LIGHT_GAMMA);
            const float decoded = light_decode((uint8_t)albedo, transfer);
            for (int intensity = 0; intensity <= 64; intensity++) {
                const float value = intensity * 0.25F;
                TEST_ASSERT_UINT8_WITHIN(MAX_ERROR, reference(exact * value, whites[w]),
                                         light_encode(decoded * value, transfer));
            }
        }
        TEST_ASSERT_EQUAL_UINT8(0, light_encode(0, transfer));
        TEST_ASSERT_UINT8_WITHIN(1, reference(4096, whites[w]), light_encode(4096, transfer));
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
            const vec3f_t n = vec3f_scale(unit, (float)length / LENGTHS);
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
        r3d_skin_clusters_lit(&f->skin, &f->bind, f->palette, 0, CLUSTERS, &single, &lights, f->out);
        for (int v = 0; v < VERTICES; v++) {
            for (int k = 0; k < R3D_SKIN_LIGHT_CHANNELS; k++) {
                const float decoded = powf((float)colors[v][k] / R3D_SKIN_LIGHT_BYTE_SCALE, R3D_SKIN_LIGHT_GAMMA);
                TEST_ASSERT_UINT8_WITHIN(2, reference(decoded * lights.ambient.x, 0), f->out[v][k]);
            }
        }
        for (int c = CLUSTERS - 1; c >= 0; c--) {
            r3d_skin_clusters_lit(&f->skin, &f->bind, f->palette, c, 1, &split, &lights, split_colors);
        }
        TEST_ASSERT_EQUAL_MEMORY(f->out, split_colors, sizeof f->out);
        TEST_ASSERT_EQUAL_MEMORY(single.positions, split.positions, sizeof positions);
        TEST_ASSERT_EQUAL_MEMORY(single.clusters, split.clusters, sizeof CLUSTER_ROWS);
        r3d_skin_clusters_lit(&f->skin, &f->bind, f->palette, CLUSTERS, 0, &split, &lights, split_colors);
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
        const float sum[] = {0.1F + colors[0].x + (count == 4 ? colors[3].x : 0),
                             0.2F + colors[0].y + (count == 4 ? colors[3].y : 0),
                             0.3F + colors[0].z + (count == 4 ? colors[3].z : 0)};
        for (int k = 0; k < R3D_SKIN_LIGHT_CHANNELS; k++) {
            const float decoded = powf((float)albedo[k] / R3D_SKIN_LIGHT_BYTE_SCALE, R3D_SKIN_LIGHT_GAMMA);
            TEST_ASSERT_UINT8_WITHIN(2, reference(decoded * sum[k], 1), got[k]);
        }
    }
    free(transfer);
}

static void
test_fused_rotations_unequal_weights_match_independent_reference(void) {
    static const int16_t positions[VERTICES][3] = {{1, 2, 3}, {4, 5, 6}, {7, 8, 9}, {3, 2, 1}};
    static const uint8_t colors[VERTICES][3] = {{91, 173, 237}, {237, 91, 173}, {173, 237, 91}, {64, 128, 255}};
    static const uint8_t weights[][JOINTS] = {{173, 82, 0, 0}, {113, 71, 43, 28}};
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
        }
        for (int v = 0; v < VERTICES; v++) {
            memcpy(f->records + v * f->skin.vertex_stride + influences, weights[influences / 2 - 1], influences);
        }
        r3d_skin_light_transfer_t* transfer = malloc(sizeof *transfer);
        TEST_ASSERT_NOT_NULL(transfer);
        r3d_skin_light_transfer_build(1, transfer);
        const r3d_skin_lights_t lights = {.count = 1,
                                          .direction = {{0, 1, 0}},
                                          .color = {{1, 0.7F, 0.3F}},
                                          .ambient = {0.1F, 0.2F, 0.3F},
                                          .transfer = transfer};
        const size_t bytes = r3d_skin_output_bytes(&f->bind);
        void* block = malloc(bytes);
        TEST_ASSERT_NOT_NULL(block);
        r3d_skin_output_t posed;
        TEST_ASSERT_TRUE(r3d_skin_output_init(&f->bind, block, bytes, &posed));
        r3d_skin_clusters_lit(&f->skin, &f->bind, f->palette, 0, CLUSTERS, &posed, &lights, f->out);
        for (int v = 0; v < VERTICES; v++) {
            float n[3] = {0};
            const int axis = v % R3D_SKIN_NORMAL_WIDTH;
            for (int j = 0; j < influences; j++) {
                for (int k = 0; k < R3D_SKIN_NORMAL_WIDTH; k++) {
                    n[k] += f->palette[j].m[k][axis] * weights[influences / 2 - 1][j] / R3D_SKIN_WEIGHT_SUM;
                }
            }
            const float facing = fmaxf(0, n[1] / sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]));
            const float channels[] = {0.1F + facing, 0.2F + 0.7F * facing, 0.3F + 0.3F * facing};
            for (int k = 0; k < R3D_SKIN_LIGHT_CHANNELS; k++) {
                const float decoded = powf((float)colors[v][k] / R3D_SKIN_LIGHT_BYTE_SCALE, R3D_SKIN_LIGHT_GAMMA);
                TEST_ASSERT_UINT8_WITHIN(2, reference(decoded * channels[k], 1), f->out[v][k]);
            }
        }
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

void
suite_skin_light(void) {
    RUN_TEST(test_direct_sum_matches_independent_float_reference);
    RUN_TEST(test_fused_rotations_unequal_weights_match_independent_reference);
    RUN_TEST(test_encode_invalid_and_range_edges);
    RUN_TEST(test_transfer_error_against_float_bake);
    RUN_TEST(test_rsqrt_dense_normals_match_exact_light_bytes);
    RUN_TEST(test_fused_decodes_albedo_and_split_matches_single);
}

SUITE_REGISTER(suite_skin_light);
