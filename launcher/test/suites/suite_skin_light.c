/* Skinned lighting contracts: axes, influence blends, cluster bounds and filtered table continuity. */
#include <stdlib.h>
#include <string.h>
#include "render/skin/r3d_skin_light.h"
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

static fixture_t
fixture(int influences) {
    fixture_t f = {.bind = {.vertex_count = VERTICES, .cluster_count = CLUSTERS, .clusters = CLUSTER_ROWS},
                   .skin = {.joint_count = JOINTS,
                            .influences = influences,
                            .vertex_count = VERTICES,
                            .vertex_stride = 2 * influences + R3D_SKIN_RECORD_TAIL}};
    for (int j = 0; j < JOINTS; j++) {
        f.palette[j] = mat4f_identity();
    }
    for (int v = 0; v < VERTICES; v++) {
        uint8_t* r = f.records + v * f.skin.vertex_stride;
        for (int j = 0; j < influences; j++) {
            r[j] = j;
            r[influences + j] = R3D_SKIN_WEIGHT_SUM / influences;
        }
        r[influences] += R3D_SKIN_WEIGHT_SUM % influences;
        r[2 * influences + v % R3D_SKIN_NORMAL_WIDTH] = R3D_SKIN_NORMAL_SCALE;
    }
    memset(f.out, SENTINEL, sizeof f.out);
    return f;
}

static void
test_axis_lambert_and_away(void) {
    fixture_t f = fixture(R3D_SKIN_INFLUENCES_MIN);
    f.skin.vertices = f.records;
    const r3d_skin_lights_t lights = {
        .count = 1, .direction = {{1, 0, 0}}, .colour = {{0.5F, 0.25F, 0.125F}}, .ambient = {0.125F, 0.125F, 0.125F}};
    r3d_skin_light_direct(&f.skin, &f.bind, f.palette, 0, CLUSTERS, &lights, false, f.out);
    const uint8_t facing[] = {159, 96, 64};
    const uint8_t ambient[] = {32, 32, 32};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(facing, f.out[0], R3D_SKIN_LIGHT_CHANNELS);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(ambient, f.out[1], R3D_SKIN_LIGHT_CHANNELS);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(ambient, f.out[2], R3D_SKIN_LIGHT_CHANNELS);
    f.records[2 * f.skin.influences] = (uint8_t)(int8_t)-R3D_SKIN_NORMAL_SCALE;
    r3d_skin_light_direct(&f.skin, &f.bind, f.palette, 0, 1, &lights, false, f.out);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(ambient, f.out[0], R3D_SKIN_LIGHT_CHANNELS);
}

static void
test_ranges_and_influence_rotations(void) {
    for (int influences = R3D_SKIN_INFLUENCES_MIN; influences <= R3D_SKIN_INFLUENCES_MAX; influences += 2) {
        fixture_t f = fixture(influences);
        f.skin.vertices = f.records;
        const r3d_skin_lights_t lights = {.count = 1, .direction = {{0, 1, 0}}, .colour = {{1, 1, 1}}};
        for (int j = 0; j < influences; j++) {
            f.palette[j].m[1][0] = (float)(j + 1) / influences;
            f.palette[j].m[0][0] = 0;
            f.palette[j].m[1][3] = 100;
        }
        r3d_skin_light_direct(&f.skin, &f.bind, f.palette, 1, 1, &lights, false, f.out);
        TEST_ASSERT_EQUAL_UINT8(SENTINEL, f.out[0][0]);
        TEST_ASSERT_EQUAL_UINT8(SENTINEL, f.out[1][0]);
        float expected = 0;
        for (int j = 0; j < influences; j++) {
            expected += (float)f.records[influences + j] * (j + 1) / influences;
        }
        TEST_ASSERT_UINT8_WITHIN(1, (uint8_t)(expected + 0.5F), f.out[3][0]);
        r3d_skin_light_direct(&f.skin, &f.bind, f.palette, 1, 1, &lights, true, f.out);
        TEST_ASSERT_EQUAL_UINT8(R3D_SKIN_LIGHT_BYTE_SCALE, f.out[3][0]);
        r3d_skin_light_direct(&f.skin, &f.bind, f.palette, CLUSTERS, 0, &lights, false, f.out);
        TEST_ASSERT_EQUAL_UINT8(SENTINEL, f.out[0][0]);
    }
}

static void
test_table_centres_edges_and_ranges(void) {
    const int size = R3D_SKIN_LIGHT_TABLE_SIZE;
    vec3f_t* directions = malloc((size_t)size * size * sizeof *directions);
    r3d_skin_light_table_t* table = malloc(sizeof *table);
    TEST_ASSERT_NOT_NULL(directions);
    TEST_ASSERT_NOT_NULL(table);
    r3d_skin_light_directions(size, directions);
    const r3d_skin_lights_t lights = {
        .count = 1, .direction = {{0, 0, 1}}, .colour = {{0.6F, 0.4F, 0.2F}}, .ambient = {0.1F, 0.1F, 0.1F}};
    r3d_skin_light_build(directions, &lights, table);
    for (int c = 0; c < size * size; c++) {
        uint8_t direct[3], lookup[3];
        r3d_skin_light_evaluate(directions[c], &lights, direct);
        r3d_skin_light_bilinear(size, table->rgb, directions[c], lookup);
        for (int k = 0; k < R3D_SKIN_LIGHT_CHANNELS; k++) {
            TEST_ASSERT_UINT8_WITHIN(1, direct[k], lookup[k]);
        }
    }
    const float epsilon = 1e-5F;
    for (int x = 1; x < size - 1; x++) {
        const float u = ((float)x + 0.5F) * 2 / size - 1;
        uint8_t left[3], right[3];
        r3d_skin_light_bilinear(size, table->rgb, vec3f_from_octahedral((vec2f_t){u - epsilon, 0.2F}), left);
        r3d_skin_light_bilinear(size, table->rgb, vec3f_from_octahedral((vec2f_t){u + epsilon, 0.2F}), right);
        for (int k = 0; k < R3D_SKIN_LIGHT_CHANNELS; k++) {
            TEST_ASSERT_UINT8_WITHIN(1, left[k], right[k]);
        }
    }
    fixture_t f = fixture(R3D_SKIN_INFLUENCES_MAX);
    f.skin.vertices = f.records;
    r3d_skin_light_table(&f.skin, &f.bind, f.palette, 1, 1, table, f.out);
    TEST_ASSERT_EQUAL_UINT8(SENTINEL, f.out[0][0]);
    TEST_ASSERT_EQUAL_UINT8(SENTINEL, f.out[1][0]);
    TEST_ASSERT_NOT_EQUAL(SENTINEL, f.out[2][0]);
    free(table);
    free(directions);
}

static void
test_fused_matches_separate_ranges(void) {
    static const int16_t positions[VERTICES][3] = {{-23, 51, 7}, {91, -31, 16}, {7, 19, -52}, {-41, 13, 37}};
    for (int influences = R3D_SKIN_INFLUENCES_MIN; influences <= R3D_SKIN_INFLUENCES_MAX; influences += 2) {
        for (int renormalise = 0; renormalise <= 1; renormalise++) {
            fixture_t* f = malloc(sizeof *f);
            TEST_ASSERT_NOT_NULL(f);
            *f = fixture(influences);
            f->skin.vertices = f->records;
            f->bind.positions = positions;
            for (int j = 0; j < JOINTS; j++) {
                f->palette[j].m[0][1] = -0.3F * (j + 1);
                f->palette[j].m[1][0] = 0.2F * (j + 1);
                f->palette[j].m[2][3] = 17.0F * j;
            }
            const r3d_skin_lights_t lights = {
                .count = R3D_SKIN_LIGHT_MAX,
                .direction = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {-1, 0, 0}},
                .colour = {{0.3F, 0.2F, 0.1F}, {0.1F, 0.2F, 0.3F}, {0.2F, 0.1F, 0.3F}, {0.1F, 0.3F, 0.2F}},
                .ambient = {0.1F, 0.2F, 0.3F}};
            const size_t bytes = r3d_skin_output_bytes(&f->bind);
            void* separate_block = calloc(1, bytes);
            void* fused_block = calloc(1, bytes);
            TEST_ASSERT_NOT_NULL(separate_block);
            TEST_ASSERT_NOT_NULL(fused_block);
            r3d_skin_output_t separate, fused;
            TEST_ASSERT_TRUE(r3d_skin_output_init(&f->bind, separate_block, bytes, &separate));
            TEST_ASSERT_TRUE(r3d_skin_output_init(&f->bind, fused_block, bytes, &fused));
            uint8_t fused_light[VERTICES][R3D_SKIN_LIGHT_CHANNELS];
            memset(fused_light, SENTINEL, sizeof fused_light);
            for (int c = CLUSTERS - 1; c >= 0; c--) {
                r3d_skin_clusters(&f->skin, &f->bind, f->palette, c, 1, &separate);
                r3d_skin_light_direct(&f->skin, &f->bind, f->palette, c, 1, &lights, renormalise, f->out);
                r3d_skin_clusters_lit(&f->skin, &f->bind, f->palette, c, 1, &fused, &lights, renormalise, fused_light);
                TEST_ASSERT_EQUAL_HEX32(asset_crc32(separate.positions, sizeof positions),
                                        asset_crc32(fused.positions, sizeof positions));
                TEST_ASSERT_EQUAL_HEX32(asset_crc32(f->out, sizeof f->out),
                                        asset_crc32(fused_light, sizeof fused_light));
                TEST_ASSERT_EQUAL_MEMORY(separate.positions, fused.positions, sizeof positions);
                TEST_ASSERT_EQUAL_MEMORY(separate.clusters, fused.clusters, sizeof CLUSTER_ROWS);
                TEST_ASSERT_EQUAL_MEMORY(f->out, fused_light, sizeof fused_light);
            }
            r3d_skin_clusters_lit(&f->skin, &f->bind, f->palette, CLUSTERS, 0, &fused, &lights, renormalise,
                                  fused_light);
            TEST_ASSERT_EQUAL_MEMORY(f->out, fused_light, sizeof fused_light);
            free(fused_block);
            free(separate_block);
            free(f);
        }
    }
}

void
suite_skin_light(void) {
    RUN_TEST(test_fused_matches_separate_ranges);
    RUN_TEST(test_axis_lambert_and_away);
    RUN_TEST(test_ranges_and_influence_rotations);
    RUN_TEST(test_table_centres_edges_and_ranges);
}

SUITE_REGISTER(suite_skin_light);
