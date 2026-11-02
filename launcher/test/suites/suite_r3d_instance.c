/*
 * Portable suite: a raster drawing several mesh instances, each at its own
 * baked model, into one picture. Each instance is one unit quad facing the
 * camera, so where a transform put it is read straight off the picture:
 * a pixel is the quad's colour, or the clear colour.
 */

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "box3f_expect.h"
#include "render_view_fixture.h"
#include "suites.h"
#include "unity.h"

#include "core/tune.h"
#include "math/geometry/box3f.h"
#include "math/linear/transformf.h"
#include "r3d_quad_mesh.h"
#include "render/r3d_lit_mesh.h"
#include "render/r3d_pipeline.h"
#include "render/raster.h"

#define SIZE   64
#define CLEAR  0x1234
#define CENTER (SIZE / 2)

typedef struct {
    int16_t positions[4][3];
    uint8_t colors[4][3];
    uint16_t triangles[2][3];
    r3d_lit_cluster_t cluster;
    r3d_lit_node_t node;
    r3d_lit_mesh_t mesh;
} quad_t;

/* A unit-sided quad in the plane z = 0, centred on (cx, 0, 0), two-sided so
 * a turn that shows its back still draws. Counter-clockwise from +z. */
static void
make_quad(quad_t* q, uint8_t red, uint8_t green, uint8_t blue, int16_t cx) {
    const int16_t xs[4] = {(int16_t)(cx - 1), (int16_t)(cx + 1), (int16_t)(cx + 1), (int16_t)(cx - 1)};
    const int16_t ys[4] = {-1, -1, 1, 1};
    for (int i = 0; i < 4; i++) {
        q->positions[i][0] = xs[i];
        q->positions[i][1] = ys[i];
        q->positions[i][2] = 0;
        q->colors[i][0] = red;
        q->colors[i][1] = green;
        q->colors[i][2] = blue;
    }
    q->triangles[0][0] = 0;
    q->triangles[0][1] = 1;
    q->triangles[0][2] = 2;
    q->triangles[1][0] = 0;
    q->triangles[1][1] = 2;
    q->triangles[1][2] = 3;
    q->cluster = (r3d_lit_cluster_t){0, 4, 0, 2, {{(int16_t)(cx - 1), -1, 0}, {(int16_t)(cx + 1), 1, 0}}, true};
    q->node = (r3d_lit_node_t){{{(int16_t)(cx - 1), -1, 0}, {(int16_t)(cx + 1), 1, 0}}, 0, 1, true};
    q->mesh = (r3d_lit_mesh_t){q->positions, q->colors, q->triangles, &q->cluster, &q->node, 4, 2, 1, 1, 1, NULL, NULL,
                               NULL,         0};
}

static const fixture_camera_t CAMERA = {{0.0F, 0.0F, -10.0F}, {0.0F, 0.0F, 1.0F}, 1.0F, 1.0F};

typedef struct {
    uint16_t* color;
    uint16_t* depth;
    raster_stats_t stats;
    void* scratch; /* the caller frees it */
} drawn_t;

static drawn_t
draw(const r3d_instance_t* instances, int count) {
    raster_t raster = {.instances = instances, .instance_count = count, .width = SIZE, .height = SIZE, .clear = CLEAR};
    drawn_t out = {.scratch = malloc(raster_scratch_bytes(&raster))};
    TEST_ASSERT_NOT_NULL(out.scratch);
    raster.scratch = out.scratch;
    const render_view_t frame_view = render_view_fixture(&CAMERA, &raster, 0);
    out.stats = raster_draw(&raster, &frame_view);
    out.color = raster_color(&raster);
    out.depth = raster_depth(&raster);
    return out;
}

/* A model that scales every axis by `scale` and moves by (x, 0, z). */
static mat4f_t
placed(float x, float z, float scale) {
    transformf_t pose = TRANSFORMF_IDENTITY;
    transformf_set_position(&pose, (vec3f_t){x, 0, z});
    transformf_set_scale(&pose, (vec3f_t){scale, scale, scale});
    return transformf_compute_matrix(&pose);
}

/* The quad is 3.2 pixels from its centre in each direction at depth 10 and
 * a half field of view of 1, and a unit's x steps 3.2 pixels along the row. */
static uint16_t
pixel(const uint16_t* color, float units_from_center) {
    return color[(CENTER * SIZE) + CENTER + (int)(units_from_center * 3.2F)];
}

static void
test_a_mesh_with_no_model_draws_where_it_is(void) {
    quad_t red;
    make_quad(&red, 255, 0, 0, 0);
    const r3d_instance_t one = {&red.mesh, NULL, NULL, NULL};
    const drawn_t drawn = draw(&one, 1);
    TEST_ASSERT_NOT_EQUAL_HEX16(CLEAR, pixel(drawn.color, 0.0F));
    TEST_ASSERT_EQUAL_HEX16(CLEAR, pixel(drawn.color, 3.0F));
    free(drawn.scratch);
}

static void
test_two_instances_appear_at_their_own_models(void) {
    quad_t red;
    quad_t green;
    make_quad(&red, 255, 0, 0, 0);
    make_quad(&green, 0, 255, 0, 0);
    const r3d_instance_t red_alone = {&red.mesh, NULL, NULL, NULL};
    const r3d_instance_t green_alone = {&green.mesh, NULL, NULL, NULL};
    drawn_t alone = draw(&red_alone, 1);
    const uint16_t red_pixel = pixel(alone.color, 0.0F);
    free(alone.scratch);
    alone = draw(&green_alone, 1);
    const uint16_t green_pixel = pixel(alone.color, 0.0F);
    free(alone.scratch);
    TEST_ASSERT_NOT_EQUAL_HEX16(red_pixel, green_pixel);

    const mat4f_t left = placed(-3.0F, 0.0F, 1.0F);
    const mat4f_t right = placed(3.0F, 0.0F, 1.0F);
    const r3d_instance_t both[] = {{&red.mesh, &left, NULL, NULL}, {&green.mesh, &right, NULL, NULL}};
    const drawn_t drawn = draw(both, 2);
    TEST_ASSERT_EQUAL_HEX16(red_pixel, pixel(drawn.color, -3.0F));
    TEST_ASSERT_EQUAL_HEX16(green_pixel, pixel(drawn.color, 3.0F));
    TEST_ASSERT_EQUAL_HEX16(CLEAR, pixel(drawn.color, 0.0F));
    free(drawn.scratch);
}

static void
test_position_ticks_keep_a_placed_instance_in_model_units(void) {
    enum { POSITION_SCALE = 8 };

    quad_t unit, ticks;
    make_quad(&unit, 255, 0, 0, 0);
    make_quad(&ticks, 255, 0, 0, 0);
    for (int i = 0; i < 4; i++) {
        for (int axis = 0; axis < 3; axis++) {
            ticks.positions[i][axis] *= POSITION_SCALE;
        }
    }
    ticks.cluster.bounds.lo = vec3s_scale(ticks.cluster.bounds.lo, POSITION_SCALE);
    ticks.cluster.bounds.hi = vec3s_scale(ticks.cluster.bounds.hi, POSITION_SCALE);
    ticks.node.bounds = ticks.cluster.bounds;
    ticks.mesh.position_scale = POSITION_SCALE;
    const mat4f_t model = placed(3.0F, 0.0F, 1.0F);
    const r3d_instance_t reference = {&unit.mesh, &model, NULL, NULL}, scaled = {&ticks.mesh, &model, NULL, NULL};
    const drawn_t expected = draw(&reference, 1);
    const drawn_t actual = draw(&scaled, 1);
    TEST_ASSERT_NOT_EQUAL_HEX16(CLEAR, pixel(actual.color, 3.0F));
    TEST_ASSERT_EQUAL_HEX16(CLEAR, pixel(actual.color, 0.0F));
    TEST_ASSERT_EQUAL_HEX16_ARRAY(expected.color, actual.color, SIZE * SIZE);
    free(expected.scratch);
    free(actual.scratch);
}

static void
test_every_lens_operation_keeps_the_affine_bottom_row(void) {
    const mat4f_t model = placed(3.0F, -2.0F, 0.5F);
    for (int quarter = 0; quarter < 4; quarter++) {
        r3d_lens_t lens;
        memset(&lens, 0x5a, sizeof lens);
        const render_view_t frame_view = render_view_fixture_at(&CAMERA, (viewport_t){SIZE, SIZE, quarter});
        r3d_lens_init(&lens, &frame_view, 8);
        for (int operation = 0; operation < 3; operation++) {
            if (operation == 1) {
                r3d_lens_fit(&lens, SIZE / 2, SIZE);
            } else if (operation == 2) {
                r3d_lens_place(&lens, &model, 8);
            }
            for (int c = 0; c < 4; c++) {
                TEST_ASSERT_EQUAL_FLOAT(c == 3 ? 1.0F : 0.0F, lens.m.m[3][c]);
            }
        }
    }
}

static void
test_a_scale_widens_an_instance_about_its_position(void) {
    quad_t red;
    make_quad(&red, 255, 0, 0, 0);
    /* 1.5 units from the centre is outside the unit quad and inside a scale of 2. */
    const mat4f_t doubled_at = placed(0.0F, 0.0F, 2.0F);
    const r3d_instance_t plain = {&red.mesh, NULL, NULL, NULL};
    const r3d_instance_t doubled = {&red.mesh, &doubled_at, NULL, NULL};
    drawn_t drawn = draw(&plain, 1);
    TEST_ASSERT_EQUAL_HEX16(CLEAR, pixel(drawn.color, 1.5F));
    free(drawn.scratch);
    drawn = draw(&doubled, 1);
    TEST_ASSERT_NOT_EQUAL_HEX16(CLEAR, pixel(drawn.color, 1.5F));
    free(drawn.scratch);
}

static void
test_a_rotation_turns_an_instance_about_its_position(void) {
    quad_t offset; /* centred 3 units to the right in its own space */
    make_quad(&offset, 0, 0, 255, 3);
    /* A half turn about y, baked: x and z negated. */
    transformf_t pose = TRANSFORMF_IDENTITY;
    transformf_set_rotation(&pose, (quatf_t){0, 1, 0, 0});
    const mat4f_t half_turn = transformf_compute_matrix(&pose);
    const r3d_instance_t as_built = {&offset.mesh, NULL, NULL, NULL};
    const r3d_instance_t turned = {&offset.mesh, &half_turn, NULL, NULL};
    drawn_t drawn = draw(&as_built, 1);
    TEST_ASSERT_NOT_EQUAL_HEX16(CLEAR, pixel(drawn.color, 3.0F));
    TEST_ASSERT_EQUAL_HEX16(CLEAR, pixel(drawn.color, -3.0F));
    free(drawn.scratch);
    drawn = draw(&turned, 1);
    TEST_ASSERT_EQUAL_HEX16(CLEAR, pixel(drawn.color, 3.0F));
    TEST_ASSERT_NOT_EQUAL_HEX16(CLEAR, pixel(drawn.color, -3.0F));
    free(drawn.scratch);
}

static void
test_a_nearer_instance_covers_a_farther_one_whichever_is_drawn_first(void) {
    quad_t red;
    quad_t green;
    make_quad(&red, 255, 0, 0, 0);
    make_quad(&green, 0, 255, 0, 0);
    const mat4f_t nearer = placed(0.0F, -2.0F, 1.0F);
    const r3d_instance_t near_first[] = {{&green.mesh, &nearer, NULL, NULL}, {&red.mesh, NULL, NULL, NULL}};
    const r3d_instance_t far_first[] = {{&red.mesh, NULL, NULL, NULL}, {&green.mesh, &nearer, NULL, NULL}};
    drawn_t drawn = draw(near_first, 2);
    const uint16_t first = pixel(drawn.color, 0.0F);
    free(drawn.scratch);
    drawn = draw(far_first, 2);
    const uint16_t second = pixel(drawn.color, 0.0F);
    free(drawn.scratch);
    TEST_ASSERT_EQUAL_HEX16(first, second);
    TEST_ASSERT_NOT_EQUAL_HEX16(CLEAR, first);
}

/* Copies at many poses: off screen, behind the camera, crossing each edge,
 * turned and scaled, in units and in ticks. */
enum { COPIES_PER_ROW = 9, COPY_ROWS = 6, COPIES = COPIES_PER_ROW * COPY_ROWS, TICKS = 8 };

static void
pose_copies(mat4f_t models[COPIES]) {
    for (int i = 0; i < COPIES; i++) {
        const int column = i % COPIES_PER_ROW, row = i / COPIES_PER_ROW;
        transformf_t pose = TRANSFORMF_IDENTITY;
        transformf_set_position(&pose, (vec3f_t){(float)(column - (COPIES_PER_ROW / 2)) * 2.7F, (float)(row % 3) - 1.0F,
                                                 (float)row * 5.5F - 13.0F});
        const float half_angle = (float)i * 0.37F;
        transformf_set_rotation(&pose, (quatf_t){0.0F, sinf(half_angle), 0.0F, cosf(half_angle)});
        const float scale = 0.5F + (float)(i % 4) * 0.5F;
        transformf_set_scale(&pose, (vec3f_t){scale, scale, scale});
        models[i] = transformf_compute_matrix(&pose);
    }
}

static void
test_world_boxes_keep_every_copy_s_pixels_depth_and_clusters(void) {
    quad_t* unit = malloc(sizeof *unit);
    quad_t* ticks = malloc(sizeof *ticks);
    mat4f_t* models = malloc(COPIES * sizeof *models);
    box3f_t* boxes = malloc(COPIES * sizeof *boxes);
    r3d_instance_t* tested = malloc(COPIES * sizeof *tested);
    r3d_instance_t* boxed = malloc(COPIES * sizeof *boxed);
    TEST_ASSERT_TRUE(unit != NULL && ticks != NULL && models != NULL && boxes != NULL && tested != NULL
                     && boxed != NULL);
    make_quad(unit, 255, 0, 0, 0);
    make_quad(ticks, 0, 0, 255, 1);
    for (int i = 0; i < 4; i++) {
        for (int axis = 0; axis < 3; axis++) {
            ticks->positions[i][axis] *= TICKS;
        }
    }
    ticks->cluster.bounds.lo = vec3s_scale(ticks->cluster.bounds.lo, TICKS);
    ticks->cluster.bounds.hi = vec3s_scale(ticks->cluster.bounds.hi, TICKS);
    ticks->node.bounds = ticks->cluster.bounds;
    ticks->mesh.position_scale = TICKS;
    pose_copies(models);
    for (int i = 0; i < COPIES; i++) {
        const r3d_lit_mesh_t* mesh = i % 2 == 0 ? &unit->mesh : &ticks->mesh;
        boxes[i] = r3d_lit_mesh_world_bounds(mesh, &models[i]);
        tested[i] = (r3d_instance_t){mesh, &models[i], NULL, NULL};
        boxed[i] = (r3d_instance_t){mesh, &models[i], &boxes[i], NULL};
    }
    const drawn_t expected = draw(tested, COPIES);
    const drawn_t actual = draw(boxed, COPIES);
    TEST_ASSERT_GREATER_THAN_INT(0, expected.stats.clusters);
    TEST_ASSERT_LESS_THAN_INT(COPIES, expected.stats.clusters); /* some copies are off screen */
    TEST_ASSERT_EQUAL_INT(expected.stats.clusters, actual.stats.clusters);
    TEST_ASSERT_EQUAL_INT(expected.stats.triangles, actual.stats.triangles);
    TEST_ASSERT_EQUAL_HEX16_ARRAY(expected.color, actual.color, SIZE * SIZE);
    TEST_ASSERT_EQUAL_HEX16_ARRAY(expected.depth, actual.depth, SIZE * SIZE);
    free(expected.scratch);
    free(actual.scratch);
    free(boxed);
    free(tested);
    free(boxes);
    free(models);
    free(ticks);
    free(unit);
}

static void
test_an_instance_whose_world_box_is_off_screen_is_skipped(void) {
    quad_t red;
    make_quad(&red, 255, 0, 0, 0);
    const box3f_t behind = {{-1.0F, -1.0F, -30.0F}, {1.0F, 1.0F, -20.0F}};
    const r3d_instance_t boxed = {&red.mesh, NULL, &behind, NULL};
    const drawn_t drawn = draw(&boxed, 1);
    TEST_ASSERT_EQUAL_INT(0, drawn.stats.clusters);
    TEST_ASSERT_EQUAL_HEX16(CLEAR, pixel(drawn.color, 0.0F));
    free(drawn.scratch);
}

/* The camera at z = -10 sees x and y within the depth from it, so at z = 0 a
 * box is outside past 10; its near plane is z = -9. Each box lies about a quad
 * that is in plain view, so only the box can skip it. */
static const struct {
    const char* side;
    box3f_t box;
} OUTSIDE[] = {
    {"right", {{11.5F, -1.0F, -1.0F}, {12.5F, 1.0F, 1.0F}}},
    {"left", {{-12.5F, -1.0F, -1.0F}, {-11.5F, 1.0F, 1.0F}}},
    {"top", {{-1.0F, 11.5F, -1.0F}, {1.0F, 12.5F, 1.0F}}},
    {"bottom", {{-1.0F, -12.5F, -1.0F}, {1.0F, -11.5F, 1.0F}}},
    {"behind the eye", {{-1.0F, -1.0F, -20.0F}, {1.0F, 1.0F, -12.0F}}},
    {"before the near plane", {{-0.1F, -0.1F, -9.6F}, {0.1F, 0.1F, -9.2F}}},
};

static void
test_a_world_box_wholly_outside_one_plane_skips_its_instance(void) {
    quad_t red;
    make_quad(&red, 255, 0, 0, 0);
    const r3d_instance_t plain = {&red.mesh, NULL, NULL, NULL};
    const drawn_t seen = draw(&plain, 1);
    TEST_ASSERT_EQUAL_INT(1, seen.stats.clusters);
    free(seen.scratch);
    for (size_t i = 0; i < sizeof OUTSIDE / sizeof OUTSIDE[0]; i++) {
        const r3d_instance_t boxed = {&red.mesh, NULL, &OUTSIDE[i].box, NULL};
        const drawn_t drawn = draw(&boxed, 1);
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, drawn.stats.clusters, OUTSIDE[i].side);
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, drawn.stats.triangles, OUTSIDE[i].side);
        TEST_ASSERT_EQUAL_HEX16_MESSAGE(CLEAR, pixel(drawn.color, 0.0F), OUTSIDE[i].side);
        free(drawn.scratch);
    }
}

/* A unit quad turned into each side's edge keeps its box straddling that
 * plane, and a box crossing the near plane is kept too. */
static void
test_a_world_box_straddling_a_plane_keeps_the_instance_and_its_pixels(void) {
    quad_t red;
    make_quad(&red, 255, 0, 0, 0);

    static const struct {
        const char* side;
        float x, y;
    } EDGES[] = {{"right", 10.0F, 0.0F}, {"left", -10.0F, 0.0F}, {"top", 0.0F, 10.0F}, {"bottom", 0.0F, -10.0F}};

    for (size_t i = 0; i < sizeof EDGES / sizeof EDGES[0]; i++) {
        transformf_t pose = TRANSFORMF_IDENTITY;
        transformf_set_position(&pose, (vec3f_t){EDGES[i].x, EDGES[i].y, 0.0F});
        const mat4f_t model = transformf_compute_matrix(&pose);
        const box3f_t box = r3d_lit_mesh_world_bounds(&red.mesh, &model);
        const r3d_instance_t unboxed = {&red.mesh, &model, NULL, NULL}, boxed = {&red.mesh, &model, &box, NULL};
        const drawn_t expected = draw(&unboxed, 1);
        const drawn_t actual = draw(&boxed, 1);
        TEST_ASSERT_EQUAL_INT_MESSAGE(1, expected.stats.clusters, EDGES[i].side);
        TEST_ASSERT_EQUAL_INT_MESSAGE(1, actual.stats.clusters, EDGES[i].side);
        TEST_ASSERT_EQUAL_INT_MESSAGE(expected.stats.triangles, actual.stats.triangles, EDGES[i].side);
        TEST_ASSERT_EQUAL_HEX16_ARRAY(expected.color, actual.color, SIZE * SIZE);
        TEST_ASSERT_EQUAL_HEX16_ARRAY(expected.depth, actual.depth, SIZE * SIZE);
        free(expected.scratch);
        free(actual.scratch);
    }
    const box3f_t across_near = {{-0.1F, -0.1F, -9.5F}, {0.1F, 0.1F, -8.5F}};
    const r3d_instance_t boxed = {&red.mesh, NULL, &across_near, NULL};
    const drawn_t drawn = draw(&boxed, 1);
    TEST_ASSERT_EQUAL_INT(1, drawn.stats.clusters);
    TEST_ASSERT_NOT_EQUAL_HEX16(CLEAR, pixel(drawn.color, 0.0F));
    free(drawn.scratch);
}

static int32_t saved_cull;
static bool cull_changed;

static void
restore_cull(void) {
    if (cull_changed) {
        (void)tune_set(tune_shared(), TUNE_NAME(render, cull), saved_cull);
        cull_changed = false;
    }
}

static void
test_with_culling_off_an_off_screen_world_box_does_not_skip(void) {
    quad_t red;
    make_quad(&red, 255, 0, 0, 0);
    const r3d_instance_t boxed = {&red.mesh, NULL, &OUTSIDE[0].box, NULL};
    const tune_entry_t* entry = tune_find(tune_shared(), TUNE_NAME(render, cull));
    TEST_ASSERT_NOT_NULL(entry);
    saved_cull = *entry->value;
    cull_changed = true;
    suite_set_test_cleanup(restore_cull);
    (void)tune_reset(tune_shared(), TUNE_NAME(render, cull));
    const drawn_t on = draw(&boxed, 1);
    TEST_ASSERT_EQUAL_INT(0, on.stats.clusters);
    free(on.scratch);
    (void)tune_set(tune_shared(), TUNE_NAME(render, cull), 0);
    const drawn_t off = draw(&boxed, 1);
    TEST_ASSERT_EQUAL_INT(1, off.stats.clusters);
    TEST_ASSERT_NOT_EQUAL_HEX16(CLEAR, pixel(off.color, 0.0F));
    free(off.scratch);
}

static void
test_a_world_box_with_no_model_is_the_root_box_in_model_units(void) {
    quad_t q;
    make_quad(&q, 255, 0, 0, 3); /* x from 2 to 4, y from -1 to 1 */
    q.mesh.position_scale = 2;
    const box3f_t want = {{1.0F, -0.5F, 0.0F}, {2.0F, 0.5F, 0.0F}};
    box3f_expect_within(0.0F, want, r3d_lit_mesh_world_bounds(&q.mesh, NULL));
}

/* An eighth of a turn about z, doubled and moved: the quad's corners land on
 * a diamond, and the box holds all four, not only the corners of its own box. */
static void
test_a_world_box_through_a_model_holds_every_transformed_corner(void) {
    quad_t q;
    make_quad(&q, 255, 0, 0, 3);
    transformf_t pose = TRANSFORMF_IDENTITY;
    const float half_angle = 0.39269908F /* an eighth of pi */;
    transformf_set_rotation(&pose, (quatf_t){0.0F, 0.0F, sinf(half_angle), cosf(half_angle)});
    transformf_set_scale(&pose, (vec3f_t){2.0F, 2.0F, 2.0F});
    transformf_set_position(&pose, (vec3f_t){5.0F, 6.0F, 7.0F});
    const mat4f_t model = transformf_compute_matrix(&pose);
    const float root_half = sqrtf(0.5F); /* x - y and x + y over both, from 1 to 5 */
    const box3f_t want = {{2.0F * root_half + 5.0F, 2.0F * root_half + 6.0F, 7.0F},
                          {10.0F * root_half + 5.0F, 10.0F * root_half + 6.0F, 7.0F}};
    box3f_expect_within(1e-4F, want, r3d_lit_mesh_world_bounds(&q.mesh, &model));
}

static void
test_a_mesh_s_bounding_sphere_is_centred_on_its_box_and_reaches_its_furthest_vertex(void) {
    static const int16_t corners[4][3] = {{0, 0, 0}, {8, 0, 0}, {8, 4, 0}, {0, 2, 0}};
    static const r3d_lit_cluster_t cluster = {0, 4, 0, 2, {{0, 0, 0}, {8, 4, 0}}, false};
    static const r3d_lit_node_t node = {{{0, 0, 0}, {8, 4, 0}}, 0, 1, true};
    r3d_lit_mesh_t mesh = r3d_quad_mesh(corners, NULL, NULL, &cluster, &node);
    mesh.position_scale = 2; /* ticks per unit: the box is 4 by 2 units */
    const spheref_t sphere = r3d_lit_mesh_bounding_sphere(&mesh);
    TEST_ASSERT_TRUE(vec3f_equal((vec3f_t){2.0F, 1.0F, 0.0F}, sphere.centre));
    TEST_ASSERT_FLOAT_WITHIN(1.0E-6F, sqrtf(5.0F), sphere.radius); /* corner (4, 2) or (0, 0): 2 by 1 from the centre */
}

void
suite_r3d_instance(void) {
    RUN_TEST(test_a_mesh_with_no_model_draws_where_it_is);
    RUN_TEST(test_two_instances_appear_at_their_own_models);
    RUN_TEST(test_position_ticks_keep_a_placed_instance_in_model_units);
    RUN_TEST(test_every_lens_operation_keeps_the_affine_bottom_row);
    RUN_TEST(test_a_scale_widens_an_instance_about_its_position);
    RUN_TEST(test_a_rotation_turns_an_instance_about_its_position);
    RUN_TEST(test_a_nearer_instance_covers_a_farther_one_whichever_is_drawn_first);
    RUN_TEST(test_world_boxes_keep_every_copy_s_pixels_depth_and_clusters);
    RUN_TEST(test_an_instance_whose_world_box_is_off_screen_is_skipped);
    RUN_TEST(test_a_world_box_wholly_outside_one_plane_skips_its_instance);
    RUN_TEST(test_a_world_box_straddling_a_plane_keeps_the_instance_and_its_pixels);
    RUN_TEST(test_with_culling_off_an_off_screen_world_box_does_not_skip);
    RUN_TEST(test_a_world_box_with_no_model_is_the_root_box_in_model_units);
    RUN_TEST(test_a_world_box_through_a_model_holds_every_transformed_corner);
    RUN_TEST(test_a_mesh_s_bounding_sphere_is_centred_on_its_box_and_reaches_its_furthest_vertex);
}

SUITE_REGISTER(suite_r3d_instance);
