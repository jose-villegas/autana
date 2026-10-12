#include <float.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include "render/skin/r3d_skinning.h"
#include "suites.h"
#include "unity.h"

enum { VERTICES = 4, CLUSTERS = 2, NODES = 3, JOINTS = 4, POSITION_SCALE = 16, FULL_RECORD = 12, SMALL_RECORD = 8 };

static const int16_t POSITIONS[][3] = {{0, 0, 0}, {16, 0, 0}, {0, 16, 0}, {16, 16, 0}};
static const uint8_t COLORS[][3] = {{10, 20, 30}, {40, 50, 60}, {70, 80, 90}, {100, 110, 120}};
static const r3d_lit_cluster_t CLUSTER_ROWS[] = {{.vertex_first = 0, .vertex_count = 2},
                                                 {.vertex_first = 2, .vertex_count = 2}};
static const r3d_lit_node_t NODE_ROWS[] = {
    {.first = 1, .count = 2}, {.first = 0, .count = 1, .leaf = true}, {.first = 1, .count = 1, .leaf = true}};
static const float INVERSE[][R3D_SKIN_MATRIX_WIDTH] = {
    {1, 0, 0, -1, 0, 1, 0, 0, 0, 0, 1, 0},
    {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0},
    {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0},
    {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0},
};

typedef struct {
    void* block;
    uint8_t* records;
    mat4f_t* palette;
    mat4f_t* model;
    r3d_skin_output_t out;
    r3d_lit_mesh_t bind;
    r3d_skin_t skin;
} fixture_t;

static fixture_t
fixture(int influences) {
    fixture_t f = {.bind = {.positions = POSITIONS,
                            .colors = COLORS,
                            .clusters = CLUSTER_ROWS,
                            .nodes = NODE_ROWS,
                            .vertex_count = VERTICES,
                            .cluster_count = CLUSTERS,
                            .node_count = NODES,
                            .position_scale = POSITION_SCALE}};
    const int stride = 2 * influences + R3D_SKIN_RECORD_TAIL;
    f.records = calloc(VERTICES, stride);
    f.palette = calloc(JOINTS, sizeof *f.palette);
    f.model = calloc(JOINTS, sizeof *f.model);
    f.block = calloc(1, r3d_skin_output_bytes(&f.bind));
    TEST_ASSERT_NOT_NULL(f.block);
    TEST_ASSERT_NOT_NULL(f.records);
    TEST_ASSERT_NOT_NULL(f.palette);
    TEST_ASSERT_NOT_NULL(f.model);
    TEST_ASSERT_TRUE(r3d_skin_output_init(&f.bind, f.block, r3d_skin_output_bytes(&f.bind), &f.out));
    for (int j = 0; j < JOINTS; j++) {
        f.model[j] = mat4f_identity();
        f.model[j].m[0][3] = (float)(j + 1);
    }
    for (int v = 0; v < VERTICES; v++) {
        uint8_t* r = f.records + v * stride;
        for (int j = 0; j < influences; j++) {
            r[j] = j;
            r[influences + j] = R3D_SKIN_WEIGHT_SUM / influences;
        }
        r[influences] += R3D_SKIN_WEIGHT_SUM % influences;
        r[2 * influences + 2] = R3D_SKIN_NORMAL_SCALE;
    }
    f.skin = (r3d_skin_t){.joint_count = JOINTS,
                          .influences = influences,
                          .vertex_count = VERTICES,
                          .inverse_binds = INVERSE,
                          .vertices = f.records,
                          .vertex_stride = stride};
    return f;
}

static void
release(fixture_t* f) {
    free(f->block);
    free(f->records);
    free(f->palette);
    free(f->model);
}

static void
test_palette_inverse_bind_and_position_ticks(void) {
    fixture_t f = fixture(R3D_SKIN_INFLUENCES_MIN);
    r3d_skin_palette(&f.skin, f.model, POSITION_SCALE, f.palette);
    TEST_ASSERT_EQUAL_FLOAT(0, f.palette[0].m[0][3]);
    TEST_ASSERT_EQUAL_FLOAT(2 * POSITION_SCALE, f.palette[1].m[0][3]);
    TEST_ASSERT_EQUAL_FLOAT(1, f.palette[0].m[0][0]);
    release(&f);
}

static void
test_skin_ranges_two_and_four_influences_and_refit(void) {
    for (int influences = R3D_SKIN_INFLUENCES_MIN; influences <= R3D_SKIN_INFLUENCES_MAX; influences += 2) {
        fixture_t f = fixture(influences);
        r3d_skin_palette(&f.skin, f.model, POSITION_SCALE, f.palette);
        r3d_skin_clusters(&f.skin, &f.bind, f.palette, 1, 1, &f.out);
        TEST_ASSERT_EQUAL_INT16(0, f.out.positions[0][0]);
        TEST_ASSERT_GREATER_THAN_INT16(0, f.out.positions[2][0]);
        r3d_skin_clusters(&f.skin, &f.bind, f.palette, 0, 1, &f.out);
        float translation = 0;
        for (int j = 0; j < influences; j++) {
            translation += f.palette[j].m[0][3] * f.records[influences + j] / (float)R3D_SKIN_WEIGHT_SUM;
        }
        const int expected = (int)(translation + 0.5F);
        TEST_ASSERT_EQUAL_INT16(expected, f.out.positions[0][0]);
        TEST_ASSERT_EQUAL_INT16(expected + POSITION_SCALE, f.out.positions[1][0]);
        TEST_ASSERT_EQUAL_PTR(COLORS, f.out.mesh.colors);
        r3d_skin_refit_nodes(&f.out);
        TEST_ASSERT_EQUAL_INT16(expected, f.out.nodes[0].lo[0]);
        TEST_ASSERT_EQUAL_INT16(expected + POSITION_SCALE, f.out.nodes[0].hi[0]);
        TEST_ASSERT_EQUAL_INT16(POSITION_SCALE, f.out.nodes[0].hi[1]);
        TEST_ASSERT_EQUAL_PTR(f.out.positions, f.out.mesh.positions);
        release(&f);
    }
}

static void
test_rotated_nonuniform_palette_transforms_off_axis_vertices(void) {
    for (int influences = R3D_SKIN_INFLUENCES_MIN; influences <= R3D_SKIN_INFLUENCES_MAX; influences += 2) {
        fixture_t f = fixture(influences);
        for (int j = 0; j < JOINTS; j++) {
            f.palette[j] = mat4f_identity();
            f.palette[j].m[0][0] = 0;
            f.palette[j].m[0][1] = -3;
            f.palette[j].m[1][0] = 2;
            f.palette[j].m[1][1] = 0;
            f.palette[j].m[2][2] = 4;
        }
        r3d_skin_clusters(&f.skin, &f.bind, f.palette, 0, CLUSTERS, &f.out);
        TEST_ASSERT_EQUAL_INT16(-3 * POSITION_SCALE, f.out.positions[3][0]);
        TEST_ASSERT_EQUAL_INT16(2 * POSITION_SCALE, f.out.positions[3][1]);
        release(&f);
    }
}

static void
test_extreme_inverse_bind_with_zero_weight_clamps_nan(void) {
    static const int16_t positions[VERTICES][3] = {{2, 0, 0}, {2, 0, 0}, {2, 0, 0}, {2, 0, 0}};
    static const float inverse[JOINTS][R3D_SKIN_MATRIX_WIDTH] = {
        {FLT_MAX, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0},
    };
    for (int influences = R3D_SKIN_INFLUENCES_MIN; influences <= R3D_SKIN_INFLUENCES_MAX; influences += 2) {
        fixture_t f = fixture(influences);
        f.bind.positions = positions;
        f.skin.inverse_binds = inverse;
        for (int j = 0; j < JOINTS; j++) {
            f.model[j] = mat4f_identity();
        }
        for (int v = 0; v < VERTICES; v++) {
            uint8_t* record = f.records + v * f.skin.vertex_stride;
            memset(record, 0, 2 * influences);
            record[influences] = R3D_SKIN_WEIGHT_SUM;
        }
        r3d_skin_palette(&f.skin, f.model, POSITION_SCALE, f.palette);
        r3d_skin_clusters(&f.skin, &f.bind, f.palette, 0, CLUSTERS, &f.out);
        for (int v = 0; v < VERTICES; v++) {
            TEST_ASSERT_EQUAL_INT16(INT16_MIN, f.out.positions[v][0]);
            TEST_ASSERT_EQUAL_INT16(0, f.out.positions[v][1]);
            TEST_ASSERT_EQUAL_INT16(0, f.out.positions[v][2]);
        }
        release(&f);
    }
}

static void
test_output_storage_refuses_null_or_short_and_retains_colourless_bind(void) {
    fixture_t f = fixture(R3D_SKIN_INFLUENCES_MIN);
    const size_t bytes = r3d_skin_output_bytes(&f.bind);
    r3d_skin_output_t out;
    TEST_ASSERT_FALSE(r3d_skin_output_init(&f.bind, NULL, bytes, &out));
    TEST_ASSERT_NULL(out.mesh.positions);
    TEST_ASSERT_FALSE(r3d_skin_output_init(&f.bind, f.block, bytes - 1, &out));
    TEST_ASSERT_NULL(out.mesh.positions);
    TEST_ASSERT_TRUE(r3d_skin_output_init(&f.bind, f.block, bytes, &out));
    TEST_ASSERT_EQUAL_PTR(f.bind.colors, out.mesh.colors);
    f.bind.colors = NULL;
    TEST_ASSERT_TRUE(r3d_skin_output_init(&f.bind, f.block, bytes, &out));
    TEST_ASSERT_NULL(out.mesh.colors);
    release(&f);
}

void
suite_skinning(void) {
    RUN_TEST(test_extreme_inverse_bind_with_zero_weight_clamps_nan);
    RUN_TEST(test_rotated_nonuniform_palette_transforms_off_axis_vertices);
    RUN_TEST(test_output_storage_refuses_null_or_short_and_retains_colourless_bind);
    RUN_TEST(test_palette_inverse_bind_and_position_ticks);
    RUN_TEST(test_skin_ranges_two_and_four_influences_and_refit);
}

SUITE_REGISTER(suite_skinning);
