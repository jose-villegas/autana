/*
 * Portable suite: render/dynamic/r3d_dynamic_mesh.h. A grid of quads built
 * at run time must come out as a lit mesh the pipeline can walk, carrying
 * each vertex's place and colour, and a source too big must leave a mesh
 * that is still whole.
 */
#include <stdlib.h>
#include <string.h>

#include "r3d_lit_mesh_expect.h"
#include "render/dynamic/r3d_dynamic_mesh.h"
#include "suites.h"
#include "test_cleanup.h"
#include "unity.h"

/* This suite's own source: a GRID x GRID quad sheet, two triangles each:
 * 68 clusters, so 9 leaves and a tree three levels deep, whose root has
 * two children and whose middle level fills the walk's eight. */
#define GRID           33
#define CORNERS        ((GRID + 1) * (GRID + 1))
#define QUADS          (GRID * GRID)
#define POSITION_SCALE 64
#define SPACING        1.25F

static vec3f_t* positions;
static uint8_t (*colors)[3];
static uint16_t (*triangles)[3];
static void* block;
static r3d_dynamic_mesh_t mesh;

static void
fixture_end(void) {
    free(positions);
    free(colors);
    free(triangles);
    free(block);
    positions = NULL;
    colors = NULL;
    triangles = NULL;
    block = NULL;
}

static r3d_dynamic_source_t
sheet(int quads) {
    return (r3d_dynamic_source_t){
        positions, (const uint8_t (*)[3])colors, CORNERS, (const uint16_t (*)[3])triangles, 2 * quads, false};
}

static bool
build(int quads) {
    const r3d_dynamic_source_t source = sheet(quads);
    return r3d_dynamic_mesh_build(&mesh, &source);
}

static void
fixture_begin(r3d_dynamic_capacity_t capacity) {
    suite_add_test_cleanup(fixture_end);
    positions = malloc(sizeof(vec3f_t) * CORNERS);
    colors = malloc(sizeof(uint8_t[3]) * CORNERS);
    triangles = malloc(sizeof(uint16_t[3]) * 2 * QUADS);
    block = malloc(r3d_dynamic_mesh_bytes(capacity));
    TEST_ASSERT_NOT_NULL(block);
    for (int i = 0; i < CORNERS; i++) {
        positions[i] = (vec3f_t){(float)(i % (GRID + 1)) * SPACING, (float)(i / (GRID + 1)) * SPACING, 0.5F};
        colors[i][0] = (uint8_t)i;
        colors[i][1] = (uint8_t)(i * 3);
        colors[i][2] = 200;
    }
    for (int q = 0; q < QUADS; q++) {
        const uint16_t a = (uint16_t)((q / GRID) * (GRID + 1) + (q % GRID));
        const uint16_t b = (uint16_t)(a + 1);
        const uint16_t c = (uint16_t)(a + GRID + 2);
        const uint16_t d = (uint16_t)(a + GRID + 1);
        memcpy(triangles[2 * q], (uint16_t[3]){a, b, c}, sizeof triangles[0]);
        memcpy(triangles[(2 * q) + 1], (uint16_t[3]){a, c, d}, sizeof triangles[0]);
    }
    TEST_ASSERT_TRUE(r3d_dynamic_mesh_init(&mesh, block, r3d_dynamic_mesh_bytes(capacity), capacity, POSITION_SCALE));
}

/* The built part alone, as a baked mesh would hold it: the spares are
 * outside the tree on purpose, so a whole-mesh check would count them. */
static r3d_lit_mesh_t
built(void) {
    r3d_lit_mesh_t view = mesh.mesh;
    view.vertex_count = mesh.built_vertices;
    view.cluster_count = mesh.built_clusters;
    return view;
}

static void
expect_valid(void) {
    const r3d_lit_mesh_t view = built();
    r3d_lit_mesh_expect_valid(&view);
}

/* A sheet's clusters share most vertices: two a triangle is room enough. */
static const r3d_dynamic_capacity_t ROOMY = {2 * 2 * QUADS, 2 * QUADS, CORNERS};

/* Each output vertex is the source vertex it copies: in ticks, same colour,
 * at the corner the raster's winding puts it. */
static void
expect_copies(void) {
    /* The build turns each triangle over: output corner k is source corner TURNED[k]. */
    static const int TURNED[3] = {0, 2, 1};
    for (int t = 0; t < mesh.mesh.triangle_count; t++) {
        for (int k = 0; k < 3; k++) {
            const int out = mesh.mesh.triangles[t][k];
            const int in = triangles[t][TURNED[k]];
            TEST_ASSERT_EQUAL_INT16((int16_t)(positions[in].x * POSITION_SCALE), mesh.mesh.positions[out][0]);
            TEST_ASSERT_EQUAL_INT16((int16_t)(positions[in].y * POSITION_SCALE), mesh.mesh.positions[out][1]);
            TEST_ASSERT_EQUAL_INT16((int16_t)(positions[in].z * POSITION_SCALE), mesh.mesh.positions[out][2]);
            TEST_ASSERT_EQUAL_MEMORY(colors[in], mesh.mesh.colors[out], 3);
        }
    }
}

static void
test_a_sheet_builds_into_a_mesh_the_pipeline_can_walk(void) {
    fixture_begin(ROOMY);
    TEST_ASSERT_TRUE(build(QUADS));
    TEST_ASSERT_EQUAL_INT(2 * QUADS, mesh.mesh.triangle_count);
    TEST_ASSERT_EQUAL_INT(((2 * QUADS) + R3D_DYNAMIC_CLUSTER_TRIANGLES - 1) / R3D_DYNAMIC_CLUSTER_TRIANGLES,
                          mesh.built_clusters);
    TEST_ASSERT_FALSE(mesh.mesh.nodes[0].leaf);
    TEST_ASSERT_FALSE(mesh.mesh.nodes[mesh.mesh.nodes[0].first].leaf); /* three levels: the walk's fan-out matters */
    expect_valid();
    expect_copies();
    for (int c = 0; c < mesh.built_clusters; c++) {
        TEST_ASSERT_LESS_OR_EQUAL_INT(R3D_DYNAMIC_CLUSTER_TRIANGLES, mesh.mesh.clusters[c].triangle_count);
    }
}

/* A cluster copies only the vertices it uses and shares them among its
 * triangles: a sheet costs near one vertex a triangle, not three. */
static void
test_clusters_share_vertices_within_themselves(void) {
    fixture_begin(ROOMY);
    TEST_ASSERT_TRUE(build(QUADS));
    TEST_ASSERT_LESS_THAN_INT(2 * mesh.mesh.triangle_count, mesh.built_vertices);
}

static void
test_a_source_too_big_leaves_a_whole_mesh(void) {
    const r3d_dynamic_capacity_t tight = {100, 2 * QUADS, CORNERS};
    fixture_begin(tight);
    TEST_ASSERT_FALSE(build(QUADS));
    TEST_ASSERT_GREATER_THAN_INT(0, mesh.mesh.triangle_count);
    TEST_ASSERT_LESS_OR_EQUAL_INT(100, mesh.built_vertices);
    expect_valid();
    expect_copies();
}

/* A rebuild replaces the last one, and an empty source leaves nothing. */
static void
test_a_rebuild_replaces_what_was_there(void) {
    fixture_begin(ROOMY);
    TEST_ASSERT_TRUE(build(QUADS));
    TEST_ASSERT_TRUE(build(3));
    TEST_ASSERT_EQUAL_INT(6, mesh.mesh.triangle_count);
    TEST_ASSERT_EQUAL_INT(1, mesh.built_clusters);
    TEST_ASSERT_EQUAL_INT(1, mesh.mesh.node_count);
    expect_valid();
    expect_copies();
    TEST_ASSERT_TRUE(build(0));
    TEST_ASSERT_EQUAL_INT(0, mesh.mesh.triangle_count);
    TEST_ASSERT_EQUAL_INT(0, mesh.mesh.node_count);
}

/* The counts a raster sizes its scratch by stay the capacities, whatever
 * each build makes, so a growing surface never asks for a larger block. */
static void
test_the_counts_a_raster_sizes_by_never_move(void) {
    fixture_begin(ROOMY);
    const int vertices = mesh.mesh.vertex_count;
    const int clusters = mesh.mesh.cluster_count;
    TEST_ASSERT_EQUAL_INT(ROOMY.vertices, vertices);
    for (int quads = 0; quads <= QUADS; quads += QUADS / 4) {
        TEST_ASSERT_TRUE(build(quads));
        TEST_ASSERT_EQUAL_INT(vertices, mesh.mesh.vertex_count);
        TEST_ASSERT_EQUAL_INT(clusters, mesh.mesh.cluster_count);
    }
}

static void
test_a_bad_capacity_or_block_is_refused(void) {
    r3d_dynamic_mesh_t m;
    uint8_t small[16];
    TEST_ASSERT_EQUAL_size_t(0, r3d_dynamic_mesh_bytes((r3d_dynamic_capacity_t){0, 10, 10}));
    TEST_ASSERT_EQUAL_size_t(0, r3d_dynamic_mesh_bytes((r3d_dynamic_capacity_t){10, 10, INT16_MAX + 1}));
    TEST_ASSERT_FALSE(r3d_dynamic_mesh_init(&m, small, sizeof small, ROOMY, POSITION_SCALE));
    TEST_ASSERT_FALSE(r3d_dynamic_mesh_init(&m, NULL, SIZE_MAX, ROOMY, POSITION_SCALE));
}

void
suite_dynamic_mesh(void) {
    RUN_TEST(test_a_sheet_builds_into_a_mesh_the_pipeline_can_walk);
    RUN_TEST(test_clusters_share_vertices_within_themselves);
    RUN_TEST(test_a_source_too_big_leaves_a_whole_mesh);
    RUN_TEST(test_a_rebuild_replaces_what_was_there);
    RUN_TEST(test_the_counts_a_raster_sizes_by_never_move);
    RUN_TEST(test_a_bad_capacity_or_block_is_refused);
}

SUITE_REGISTER(suite_dynamic_mesh);
