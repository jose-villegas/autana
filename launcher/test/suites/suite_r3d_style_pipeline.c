#ifndef DEVICE_BUILD
#define r3d_span_small_fill_resolve  constant_resolve
#define r3d_span_small_fill_default  constant_default
#define r3d_span_triangle            constant_r3d_span_triangle
#define r3d_span_triangle_solid      constant_r3d_span_triangle_solid
#define r3d_span_triangle_write      constant_r3d_span_triangle_write
#define r3d_span_hidden              constant_r3d_span_hidden
#define r3d_span_plane_bound         constant_r3d_span_plane_bound
#define r3d_span_plane_in_range      constant_r3d_span_plane_in_range
#define r3d_span_stop_after          constant_r3d_span_stop_after
#define r3d_lens_init                constant_r3d_lens_init
#define r3d_lens_fit                 constant_r3d_lens_fit
#define r3d_lens_place               constant_r3d_lens_place
#define r3d_pipeline_work_bytes      constant_r3d_pipeline_work_bytes
#define r3d_pipeline_cull            constant_r3d_pipeline_cull
#define r3d_pipeline_transform       constant_r3d_pipeline_transform
#define r3d_pipeline_transform_split constant_r3d_pipeline_transform_split
#define r3d_pipeline_draw_split      constant_r3d_pipeline_draw_split
#define r3d_pipeline_draw            constant_r3d_pipeline_draw
#define r3d_pipeline_cluster_hidden  constant_r3d_pipeline_cluster_hidden

#include <stdlib.h>
#include <string.h>
#include "asset/asset_pack.h"
#include "hash/crc32.h"
#include "render/r3d_pipeline.c"
#include "render/r3d_span.c"
#include "suites.h"
#include "unity.h"

#define TEST_SIDE   8
#define TEST_PIXELS (TEST_SIDE * TEST_SIDE)

static uint16_t* pixels;
static r3d_pipeline_work_t* work;

typedef struct {
    r3d_span_small_fill_t resolved;
    r3d_pipeline_vertex_t transformed[6];
    r3d_span_vertex_t projected[4], front[3];
    r3d_span_target_t expected, actual;
} clipped_fixture_t;

static clipped_fixture_t* clipped;

/* Meshes of flat triangles seen at their own pixels, drawn through the
 * pipeline. */
#define SCENE_W         64
#define SCENE_H         48
#define SCENE_CLUSTERS  5
#define SCENE_VERTICES  17
#define SCENE_TRIANGLES 7
#define SCENE_DRAWS     2

typedef struct {
    int16_t positions[SCENE_VERTICES][3];
    uint8_t colors[SCENE_VERTICES][3];
    uint16_t triangles[SCENE_TRIANGLES][3];
    r3d_lit_cluster_t clusters[SCENE_CLUSTERS];
    r3d_pipeline_vertex_t cs[SCENE_VERTICES];
    r3d_pipeline_rows_t rows[SCENE_CLUSTERS];
    int vertices, triangles_made, clusters_made;
} scene_t;

/* Colour, depth and tag: 16-bit planes of SCENE_W by SCENE_H each, per draw. */
#define PLANES_PER_DRAW 3

static scene_t* scene;
static uint16_t* planes;

static void
cleanup(void) {
    free(scene);
    scene = NULL;
    free(planes);
    planes = NULL;
    free(clipped);
    clipped = NULL;
    free(work);
    work = NULL;
    free(pixels);
    pixels = NULL;
}

static void
test_styles_match_pipeline_colours(void) {
    pixels = calloc(TEST_PIXELS * 4, sizeof(*pixels));
    suite_set_test_cleanup(cleanup);
    TEST_ASSERT_NOT_NULL(pixels);
    work = malloc(r3d_pipeline_work_bytes());
    TEST_ASSERT_NOT_NULL(work);
    TEST_ASSERT_EQUAL_UINT(8, sizeof(r3d_pipeline_vertex_t));
    const int16_t positions[3][3] = {{0, 0, 2}, {1, 0, 2}, {0, 1, 2}};
    uint8_t colours[3][3] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}};
    const uint16_t triangle[3] = {0, 1, 2};
    const r3d_lit_cluster_t cluster = {.vertex_count = 3};
    const r3d_lit_mesh_t mesh = {.positions = positions, .colors = (const uint8_t (*)[3])colours};
    const r3d_lens_t lens = {.m = MAT4F_IDENTITY,
                             .project = {1.0F, -1.0F, 0.0F, 0.0F, 1.0F},
                             .near_subpixels = 1.0F / R3D_SUBPIXEL,
                             .snap_cx = R3D_SNAP_BIAS,
                             .snap_cy = R3D_SNAP_BIAS};
    r3d_pipeline_vertex_t transformed[3];
    (void)transform_cluster(&mesh, &lens, &cluster, transformed);
    r3d_span_vertex_t reference[3];
    for (int k = 0; k < 3; k++) {
        transformed[k].sx = k == 1 ? TEST_SIDE * R3D_SUBPIXEL : 0;
        transformed[k].sy = k == 2 ? TEST_SIDE * R3D_SUBPIXEL : 0;
        reference[k] = (r3d_span_vertex_t){transformed[k].sx, transformed[k].sy, transformed[k].iz,
                                           colours[k][0],     colours[k][1],     colours[k][2]};
    }
    r3d_span_target_t expected = r3d_span_target(pixels, pixels + TEST_PIXELS, TEST_SIDE, 0, TEST_SIDE);
    r3d_span_target_t actual =
        r3d_span_target(pixels + TEST_PIXELS * 2, pixels + TEST_PIXELS * 3, TEST_SIDE, 0, TEST_SIDE);
    const r3d_pipeline_vertex_t* vertices[3] = {transformed, transformed + 1, transformed + 2};
    for (small_fill_style_t style = SMALL_FILL_FLAT; style < SMALL_FILL_STYLE_COUNT; style++) {
        r3d_span_small_fill_t resolved;
        r3d_span_small_fill_resolve(&resolved, (small_fill_t){TEST_SIDE, 0, style});
        expected.small_fill = actual.small_fill = &resolved;
        memset(pixels, 0, TEST_PIXELS * 4 * sizeof(*pixels));
        r3d_span_triangle(&expected, reference, reference + 1, reference + 2);
        draw_in_front(&mesh, vertices, triangle, NULL, true, &actual, false, work);
        TEST_ASSERT_EQUAL_MEMORY(pixels, pixels + TEST_PIXELS * 2, TEST_PIXELS * 2 * sizeof(*pixels));
        int coloured = 0;
        for (int i = 0; i < TEST_PIXELS; i++) {
            coloured += pixels[i] != 0;
        }
        TEST_ASSERT_GREATER_THAN(0, coloured);
    }
}

static void
test_clipped_and_mixed_clusters_keep_the_target_style(void) {
    pixels = calloc(TEST_PIXELS * 4, sizeof(*pixels));
    suite_set_test_cleanup(cleanup);
    TEST_ASSERT_NOT_NULL(pixels);
    work = malloc(r3d_pipeline_work_bytes());
    TEST_ASSERT_NOT_NULL(work);
    static const int16_t positions[6][3] = {{1, 1, 0}, {32, 0, 4}, {0, 32, 4}, {0, 0, 4}, {24, 0, 4}, {0, 24, 4}};
    static const uint8_t colors[6][3] = {{255, 0, 0},  {0, 255, 0},    {0, 0, 255},
                                         {32, 80, 96}, {128, 112, 48}, {224, 160, 144}};
    static const uint16_t triangles[2][3] = {{0, 1, 2}, {3, 4, 5}};
    const r3d_lit_cluster_t cluster = {.vertex_count = 6, .triangle_count = 2, .double_sided = true};
    const r3d_lit_mesh_t mesh = {
        .positions = positions, .colors = colors, .triangles = triangles, .clusters = &cluster};
    const r3d_lens_t lens = {.m = MAT4F_IDENTITY,
                             .project = {1.0F, -1.0F, 0.0F, 0.0F, 1.0F},
                             .near_subpixels = 1.0F / R3D_SUBPIXEL,
                             .snap_cx = R3D_SNAP_BIAS,
                             .snap_cy = R3D_SNAP_BIAS};
    clipped = malloc(sizeof(*clipped));
    TEST_ASSERT_NOT_NULL(clipped);
    r3d_pipeline_vertex_t* transformed = clipped->transformed;
    (void)transform_cluster(&mesh, &lens, &cluster, transformed);
    build_guard_planes(&lens, work);
    r3d_span_target_t* expected = &clipped->expected;
    r3d_span_target_t* actual = &clipped->actual;
    *expected = r3d_span_target(pixels, pixels + TEST_PIXELS, TEST_SIDE, 0, TEST_SIDE);
    *actual = r3d_span_target(pixels + TEST_PIXELS * 2, pixels + TEST_PIXELS * 3, TEST_SIDE, 0, TEST_SIDE);
    const clip_vertex_t polygon[4] = {{8.75F, 0.75F, 1, 191.25F, 63.75F, 0},
                                      {32, 0, 4, 0, 255, 0},
                                      {0, 32, 4, 0, 0, 255},
                                      {0.75F, 8.75F, 1, 191.25F, 0, 63.75F}};
    uint32_t checksums[SMALL_FILL_STYLE_COUNT];
    for (small_fill_style_t style = SMALL_FILL_FLAT; style < SMALL_FILL_STYLE_COUNT; style++) {
        r3d_span_small_fill_t* resolved = &clipped->resolved;
        r3d_span_small_fill_resolve(resolved, (small_fill_t){SMALL_FILL_CEILING_FAST, 0, style});
        expected->small_fill = actual->small_fill = resolved;
        for (int mixed = 0; mixed < 2; mixed++) {
            memset(pixels, 0, TEST_PIXELS * 4 * sizeof(*pixels));
            r3d_span_vertex_t* projected = clipped->projected;
            for (int k = 0; k < 4; k++) {
                projected[k] = project(&lens, &polygon[k]);
            }
            r3d_span_triangle(expected, projected, projected + 1, projected + 2);
            r3d_span_triangle(expected, projected, projected + 2, projected + 3);
            if (mixed) {
                r3d_span_vertex_t* front = clipped->front;
                for (int k = 0; k < 3; k++) {
                    const r3d_pipeline_vertex_t* v = transformed + k + 3;
                    front[k] =
                        (r3d_span_vertex_t){v->sx, v->sy, v->iz, colors[k + 3][0], colors[k + 3][1], colors[k + 3][2]};
                }
                r3d_span_triangle(expected, front, front + 1, front + 2);
            }
            draw_triangles(&mesh, &lens, &cluster, cluster.triangle_first, cluster.triangle_first + (mixed ? 2 : 1),
                           transformed, actual, false, work);
            TEST_ASSERT_EQUAL_MEMORY(pixels, pixels + TEST_PIXELS * 2, TEST_PIXELS * 2 * sizeof(*pixels));
            int covered = 0;
            for (int i = 0; i < TEST_PIXELS; i++) {
                covered += pixels[TEST_PIXELS + i] != R3D_DEPTH_EMPTY;
            }
            TEST_ASSERT_GREATER_THAN(0, covered);
        }
        checksums[style] = crc32_update(0, pixels, TEST_PIXELS * sizeof(*pixels));
        for (int previous = SMALL_FILL_FLAT; previous < (int)style; previous++) {
            TEST_ASSERT_NOT_EQUAL(checksums[previous], checksums[style]);
        }
    }
}

/* One draw on `work` testing R3D_HIDDEN_PAYS_ONE_IN clusters, `hidden` of
 * them hidden: whether it looked for them. */
static bool
hidden_draw_of(int hidden) {
    begin_hidden_draw(work);
    const bool looked = work->test_hidden;
    if (looked) {
        work->payoff += (hidden * R3D_HIDDEN_PAYS_ONE_IN) - R3D_HIDDEN_PAYS_ONE_IN;
    }
    end_hidden_draw(work);
    return looked;
}

static void
test_the_hidden_test_pauses_while_it_does_not_pay_and_resumes_when_it_does(void) {
    work = malloc(r3d_pipeline_work_bytes());
    suite_set_test_cleanup(cleanup);
    TEST_ASSERT_NOT_NULL(work);
    for (int fresh = R3D_HIDDEN_CYCLE_DRAWS; fresh <= UINT8_MAX; fresh++) {
        work->hidden_draw = (uint8_t)fresh;
        TEST_ASSERT_TRUE(hidden_draw_of(0));
        TEST_ASSERT_EQUAL(1, work->hidden_draw);
    }
    work->hidden_draw = 0;
    for (int cycle = 0; cycle < 2; cycle++) {
        for (int draw = 0; draw < R3D_HIDDEN_WINDOW_DRAWS; draw++) {
            TEST_ASSERT_TRUE(hidden_draw_of(0));
        }
        for (int draw = R3D_HIDDEN_WINDOW_DRAWS; draw < R3D_HIDDEN_CYCLE_DRAWS; draw++) {
            TEST_ASSERT_FALSE(hidden_draw_of(1));
        }
    }
    TEST_ASSERT_TRUE(hidden_draw_of(0));
}

/* A window pays as a whole: one draw hiding plenty carries the rest, and
 * hiding too few clusters over the window pauses the test. */
static void
test_the_hidden_test_pays_over_its_window(void) {
    work = malloc(r3d_pipeline_work_bytes());
    suite_set_test_cleanup(cleanup);
    TEST_ASSERT_NOT_NULL(work);
    work->hidden_draw = 0;
    TEST_ASSERT_TRUE(hidden_draw_of(R3D_HIDDEN_WINDOW_DRAWS));
    for (int draw = 1; draw < R3D_HIDDEN_WINDOW_DRAWS; draw++) {
        TEST_ASSERT_TRUE(hidden_draw_of(0));
    }
    TEST_ASSERT_TRUE(hidden_draw_of(1));
    for (int draw = 1; draw < R3D_HIDDEN_WINDOW_DRAWS; draw++) {
        TEST_ASSERT_TRUE(hidden_draw_of(0));
    }
    TEST_ASSERT_FALSE(hidden_draw_of(0));
}

/* Positions are pixel * R3D_SUBPIXEL * z ticks, so the lens maps them back to the pixel. */
static r3d_lens_t
pixel_lens(void) {
    const float s = 1.0F / (float)R3D_SUBPIXEL;
    return (r3d_lens_t){{{{s, 0, 0, 0}, {0, s, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}}},
                        {1.0F, -1.0F, 0.0F, 0.0F, 0.5F},
                        SCENE_W,
                        SCENE_H,
                        R3D_SNAP_BIAS,
                        R3D_SNAP_BIAS,
                        0.5F / (float)R3D_SUBPIXEL};
}

/* One cluster: a fan over `count` pixel corners at `z` ticks, all one colour. */
static void
scene_add(const float (*corners)[2], int count, int z, const uint8_t rgb[3]) {
    r3d_lit_cluster_t* c = &scene->clusters[scene->clusters_made++];
    *c = (r3d_lit_cluster_t){.vertex_first = (uint16_t)scene->vertices,
                             .vertex_count = (uint16_t)count,
                             .triangle_first = (uint16_t)scene->triangles_made,
                             .triangle_count = (uint16_t)(count - 2),
                             .double_sided = true};
    for (int k = 0; k < count; k++) {
        const int v = scene->vertices++;
        scene->positions[v][0] = (int16_t)(corners[k][0] * R3D_SUBPIXEL * (float)z);
        scene->positions[v][1] = (int16_t)(corners[k][1] * R3D_SUBPIXEL * (float)z);
        scene->positions[v][2] = (int16_t)z;
        memcpy(scene->colors[v], rgb, 3);
    }
    for (int k = 1; k + 1 < count; k++) {
        uint16_t* t = scene->triangles[scene->triangles_made++];
        t[0] = c->vertex_first;
        t[1] = (uint16_t)(c->vertex_first + k);
        t[2] = (uint16_t)(c->vertex_first + k + 1);
    }
}

static r3d_lit_mesh_t
scene_mesh(void) {
    return (r3d_lit_mesh_t){.positions = scene->positions,
                            .colors = scene->colors,
                            .triangles = scene->triangles,
                            .clusters = scene->clusters,
                            .vertex_count = scene->vertices,
                            .triangle_count = scene->triangles_made,
                            .cluster_count = scene->clusters_made,
                            .position_scale = 1};
}

static void
scene_open(void) {
    suite_set_test_cleanup(cleanup);
    scene = calloc(1, sizeof(*scene));
    planes = calloc((size_t)PLANES_PER_DRAW * SCENE_DRAWS * SCENE_W * SCENE_H, sizeof(*planes));
    work = malloc(r3d_pipeline_work_bytes());
    TEST_ASSERT_NOT_NULL(scene);
    TEST_ASSERT_NOT_NULL(planes);
    TEST_ASSERT_NOT_NULL(work);
}

enum { PLANE_COLOUR, PLANE_DEPTH, PLANE_TAG };

/* The colour, depth or tag plane of draw `run`. */
static uint16_t*
plane(int run, int kind) {
    return planes + ((size_t)((run * PLANES_PER_DRAW) + kind) * SCENE_W * SCENE_H);
}

/* Writes its value where the fill's depth is the one in the depth plane. */
static void
tag_where_won(const r3d_span_writer_t* writer, const gfx_target_t* rows, int y, int x_first, int x_last, int32_t z,
              int32_t dz) {
    const image_t* attachment = &rows->attachment[writer->attachment];
    const uint16_t* depth = gfx_target_depth(rows, y);
    uint16_t* tag = (uint16_t*)((char*)attachment->pixels + ((size_t)(y - rows->first_row) * attachment->pitch));
    for (int x = x_first; x <= x_last; x++, z += dz) {
        if ((uint16_t)(z >> R3D_DEPTH_SHIFT) == depth[x]) {
            tag[x] = (uint16_t)writer->value;
        }
    }
}

#define TAG_BASE 1000

/* Rows [0, rows) of draw `run`'s planes, with a tag attachment when `writer` is given. */
static r3d_span_target_t
scene_target(int run, int rows, r3d_span_writer_t* writer) {
    r3d_span_target_t target = r3d_span_target(plane(run, PLANE_COLOUR), plane(run, PLANE_DEPTH), SCENE_W, 0, rows);
    if (writer != NULL) {
        *writer = (r3d_span_writer_t){.span = tag_where_won,
                                      .attachment = GFX_ATTACHMENT_FURTHER,
                                      .step = R3D_SPAN_STEP_CLUSTER,
                                      .value = TAG_BASE};
        target.rows.attachment[GFX_ATTACHMENT_FURTHER] =
            (image_t){plane(run, PLANE_TAG), IMAGE_U16, SCENE_W, rows, IMAGE_PITCH(IMAGE_U16, SCENE_W)};
        target.rows.count = GFX_ATTACHMENT_FURTHER + 1;
        target.writers = writer;
        target.writer_count = 1;
    }
    return target;
}

/* Five clusters, drawn nearest first. The wall is a quad; two lie wholly
 * behind it, one only partly, and one behind nothing. */
enum { WALL, BEHIND_WALL_NEAR, BEHIND_WALL_FAR, PARTLY_BEHIND, CLEAR };

static const uint16_t NEAREST_FIRST[SCENE_CLUSTERS] = {WALL, CLEAR, BEHIND_WALL_NEAR, BEHIND_WALL_FAR, PARTLY_BEHIND};
#define HIDDEN_CLUSTERS 2

static void
build_wall_scene(void) {
    static const float wall[4][2] = {{2, 2}, {40, 2}, {40, 30}, {2, 30}};
    static const float near_behind[3][2] = {{4, 4}, {36, 4}, {4, 28}};
    static const float far_behind[3][2] = {{10, 8}, {30, 8}, {20, 24}};
    static const float partly[3][2] = {{30, 10}, {60, 10}, {45, 28}};
    static const float clear[4][2] = {{44, 34}, {62, 34}, {62, 46}, {44, 46}};
    static const uint8_t red[3] = {255, 0, 0}, white[3] = {255, 255, 255}, blue[3] = {0, 0, 255},
                         green[3] = {0, 255, 0}, yellow[3] = {255, 255, 0};
    scene_add(wall, 4, 2, red);
    scene_add(near_behind, 3, 3, white);
    scene_add(far_behind, 3, 4, blue);
    scene_add(partly, 3, 4, green);
    scene_add(clear, 4, 3, yellow);
}

/* A pixel of the partly covered cluster outside the wall, and pixels inside
 * the wall where each cluster behind it would have drawn. */
#define PARTLY_OPEN_X 50
#define PARTLY_OPEN_Y 15
#define BEHIND_FAR_X  20
#define BEHIND_FAR_Y  12
#define BEHIND_NEAR_X 10
#define BEHIND_NEAR_Y 10

static int
pixel_index(int x, int y) {
    return (y * SCENE_W) + x;
}

/* The scene drawn with the hidden test on, and again with it off, ends the
 * same in every plane; with it on, the clusters wholly behind the wall were
 * skipped and the one partly behind it was drawn. */
static void
scene_draws_alike_with_the_hidden_test_on_and_off(bool writes) {
    scene_open();
    build_wall_scene();
    const r3d_lit_mesh_t mesh = scene_mesh();
    const r3d_lens_t lens = pixel_lens();
    r3d_pipeline_transform(&mesh, &lens, NEAREST_FIRST, SCENE_CLUSTERS, scene->cs, scene->rows);
    r3d_span_writer_t writers[2];
    for (int run = 0; run < 2; run++) {
        const r3d_span_target_t target = scene_target(run, SCENE_H, writes ? &writers[run] : NULL);
        work->hidden_draw = run == 0 ? 0 : R3D_HIDDEN_WINDOW_DRAWS;
        r3d_pipeline_draw(&mesh, &lens, NEAREST_FIRST, SCENE_CLUSTERS, scene->cs, scene->rows, &target, work);
        TEST_ASSERT_EQUAL(run == 0, work->test_hidden);
    }
    const size_t bytes = (size_t)SCENE_W * SCENE_H * sizeof(uint16_t);
    for (int kind = PLANE_COLOUR; kind <= (writes ? PLANE_TAG : PLANE_DEPTH); kind++) {
        TEST_ASSERT_EQUAL_MEMORY(plane(0, kind), plane(1, kind), bytes);
    }
    TEST_ASSERT_EQUAL_INT32((HIDDEN_CLUSTERS * R3D_HIDDEN_PAYS_ONE_IN) - SCENE_CLUSTERS, (int32_t)work->payoff);
    const int open = pixel_index(PARTLY_OPEN_X, PARTLY_OPEN_Y);
    TEST_ASSERT_NOT_EQUAL_UINT16(R3D_DEPTH_EMPTY, plane(0, PLANE_DEPTH)[open]);
    TEST_ASSERT_NOT_EQUAL_UINT16(0, plane(0, PLANE_COLOUR)[open]);
    if (writes) {
        TEST_ASSERT_EQUAL_UINT16(TAG_BASE + PARTLY_BEHIND, plane(0, PLANE_TAG)[open]);
        TEST_ASSERT_EQUAL_UINT16(TAG_BASE + WALL, plane(0, PLANE_TAG)[pixel_index(BEHIND_FAR_X, BEHIND_FAR_Y)]);
        TEST_ASSERT_EQUAL_UINT16(TAG_BASE + WALL, plane(0, PLANE_TAG)[pixel_index(BEHIND_NEAR_X, BEHIND_NEAR_Y)]);
    }
}

static void
test_the_hidden_test_changes_no_pixel_of_a_reading_draw(void) {
    scene_draws_alike_with_the_hidden_test_on_and_off(false);
}

static void
test_the_hidden_test_changes_no_pixel_of_a_writing_draw(void) {
    scene_draws_alike_with_the_hidden_test_on_and_off(true);
}

/* Three clusters for a window of the top GATE_WINDOW_ROWS rows: two inside
 * it, one wholly below. A draw lists clusters by index, repeats allowed; the
 * walled one is hidden behind depth laid over its box. */
#define GATE_WINDOW_ROWS 24

enum { GATE_WALLED, GATE_OPEN, GATE_BELOW, GATE_CLUSTERS };

static void
build_gate_scene(void) {
    static const float walled[3][2] = {{4, 4}, {16, 4}, {4, 16}};
    static const float open[3][2] = {{24, 4}, {36, 4}, {24, 16}};
    static const float below[3][2] = {{4, 30}, {16, 30}, {4, 44}};
    static const uint8_t grey[3] = {128, 128, 128};
    scene_add(walled, 3, 2, grey);
    scene_add(open, 3, 2, grey);
    scene_add(below, 3, 2, grey);
}

/* The walled cluster's box and a margin round it. */
#define GATE_WALL_FIRST 3
#define GATE_WALL_END   18

/* A fresh scene with depth nearer than the walled cluster's limit over its box. */
static void
gate_open(void) {
    scene_open();
    build_gate_scene();
    const r3d_lit_mesh_t mesh = scene_mesh();
    const r3d_lens_t lens = pixel_lens();
    const uint16_t each[GATE_CLUSTERS] = {GATE_WALLED, GATE_OPEN, GATE_BELOW};
    r3d_pipeline_transform(&mesh, &lens, each, GATE_CLUSTERS, scene->cs, scene->rows);
    for (int y = GATE_WALL_FIRST; y < GATE_WALL_END; y++) {
        for (int x = GATE_WALL_FIRST; x < GATE_WALL_END; x++) {
            plane(0, PLANE_DEPTH)[pixel_index(x, y)] = (uint16_t)(scene->rows[GATE_WALLED].depth + 1);
        }
    }
    work->hidden_draw = 0;
}

static void
gate_draw(const uint16_t* list, int count) {
    const r3d_lit_mesh_t mesh = scene_mesh();
    const r3d_lens_t lens = pixel_lens();
    const r3d_span_target_t target = scene_target(0, GATE_WINDOW_ROWS, NULL);
    r3d_pipeline_draw(&mesh, &lens, list, count, scene->cs, scene->rows, &target, work);
}

#define GATE_CLUSTERS_PER_DRAW 8
#define GATE_LIST_MAX          (GATE_CLUSTERS_PER_DRAW + GATE_CLUSTERS_PER_DRAW + GATE_CLUSTERS_PER_DRAW)

/* One window of R3D_HIDDEN_WINDOW_DRAWS draws, each listing
 * GATE_CLUSTERS_PER_DRAW open clusters and `below` more under the window; in
 * the first draw `walled` of the open ones are the walled cluster and `extra`
 * more open ones are added. */
static void
gate_window(int walled, int extra, int below) {
    uint16_t list[GATE_LIST_MAX];
    for (int draw = 0; draw < R3D_HIDDEN_WINDOW_DRAWS; draw++) {
        int n = 0;
        for (int k = 0; k < GATE_CLUSTERS_PER_DRAW; k++) {
            list[n++] = draw == 0 && k < walled ? GATE_WALLED : GATE_OPEN;
        }
        for (int k = 0; k < (draw == 0 ? extra : 0); k++) {
            list[n++] = GATE_OPEN;
        }
        for (int k = 0; k < below; k++) {
            list[n++] = GATE_BELOW;
        }
        gate_draw(list, n);
    }
}

/* Whether a one-cluster draw looks for hidden clusters. */
static bool
gate_next_draw_tests(void) {
    const uint16_t list[1] = {GATE_OPEN};
    gate_draw(list, 1);
    return work->test_hidden;
}

/* A window testing R3D_HIDDEN_PAYS_ONE_IN clusters, one of them hidden, has a
 * payoff of exactly 0, which keeps testing. */
static void
test_a_window_hiding_one_in_the_payoff_ratio_keeps_the_hidden_test_running(void) {
    gate_open();
    TEST_ASSERT_EQUAL_INT(R3D_HIDDEN_PAYS_ONE_IN, R3D_HIDDEN_WINDOW_DRAWS * GATE_CLUSTERS_PER_DRAW);
    gate_window(1, 0, 0);
    TEST_ASSERT_EQUAL_INT32(0, (int32_t)work->payoff);
    TEST_ASSERT_EQUAL_UINT8(0, work->hidden_draw);
    TEST_ASSERT_TRUE(gate_next_draw_tests());
}

/* One more cluster tested for the same one hidden: payoff -1 pauses the test. */
static void
test_a_window_one_cluster_short_of_the_payoff_ratio_pauses_the_hidden_test(void) {
    gate_open();
    gate_window(1, 1, 0);
    TEST_ASSERT_EQUAL_INT32(-1, (int32_t)work->payoff);
    TEST_ASSERT_FALSE(gate_next_draw_tests());
}

/* With nothing hidden the rest of the cycle skips the test, then it starts again. */
static void
test_a_window_hiding_nothing_pauses_the_hidden_test_until_the_cycle_ends(void) {
    gate_open();
    gate_window(0, 0, 0);
    TEST_ASSERT_EQUAL_INT32(-R3D_HIDDEN_PAYS_ONE_IN, (int32_t)work->payoff);
    for (int draw = R3D_HIDDEN_WINDOW_DRAWS; draw < R3D_HIDDEN_CYCLE_DRAWS; draw++) {
        TEST_ASSERT_FALSE(gate_next_draw_tests());
    }
    TEST_ASSERT_TRUE(gate_next_draw_tests());
}

/* Clusters below the window are skipped before any test and counted in none:
 * the window that pays still pays with them listed. */
static void
test_clusters_missing_the_window_are_not_counted_as_tested(void) {
    gate_open();
    gate_window(1, 0, GATE_CLUSTERS_PER_DRAW);
    TEST_ASSERT_EQUAL_INT32(0, (int32_t)work->payoff);
    TEST_ASSERT_TRUE(gate_next_draw_tests());
    for (int i = GATE_WINDOW_ROWS * SCENE_W; i < SCENE_W * SCENE_H; i++) {
        TEST_ASSERT_EQUAL_UINT16(R3D_DEPTH_EMPTY, plane(0, PLANE_DEPTH)[i]);
    }
}

void
run_r3d_style_pipeline_suite(void) {
    RUN_TEST(test_clipped_and_mixed_clusters_keep_the_target_style);
    RUN_TEST(test_styles_match_pipeline_colours);
    RUN_TEST(test_the_hidden_test_pauses_while_it_does_not_pay_and_resumes_when_it_does);
    RUN_TEST(test_the_hidden_test_pays_over_its_window);
    RUN_TEST(test_the_hidden_test_changes_no_pixel_of_a_reading_draw);
    RUN_TEST(test_the_hidden_test_changes_no_pixel_of_a_writing_draw);
    RUN_TEST(test_a_window_hiding_one_in_the_payoff_ratio_keeps_the_hidden_test_running);
    RUN_TEST(test_a_window_one_cluster_short_of_the_payoff_ratio_pauses_the_hidden_test);
    RUN_TEST(test_a_window_hiding_nothing_pauses_the_hidden_test_until_the_cycle_ends);
    RUN_TEST(test_clusters_missing_the_window_are_not_counted_as_tested);
}

SUITE_REGISTER(run_r3d_style_pipeline_suite);
#endif
