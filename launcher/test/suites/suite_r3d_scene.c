/*
 * Portable suite: a raster drawing several mesh instances, each at its own
 * baked placement, into one picture. Each instance is one unit quad facing the
 * camera, so where a transform put it is read straight off the picture:
 * a pixel is the quad's colour, or the clear colour.
 */

#include <stdint.h>
#include <stdlib.h>

#include "suites.h"
#include "unity.h"

#include "render/r3d_pipeline.h"
#include "render/r3d_scene.h"
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
    q->cluster = (r3d_lit_cluster_t){0, 4, 0, 2, {(int16_t)(cx - 1), -1, 0}, {(int16_t)(cx + 1), 1, 0}, true};
    q->node = (r3d_lit_node_t){{(int16_t)(cx - 1), -1, 0}, {(int16_t)(cx + 1), 1, 0}, 0, 1, true};
    q->mesh = (r3d_lit_mesh_t){q->positions, q->colors, q->triangles, &q->cluster, &q->node, 4, 2, 1, 1, 1, NULL};
}

static const camera_t CAMERA = {{0.0F, 0.0F, 10.0F}, {0.0F, 0.0F, -1.0F}, 1.0F, 1.0F};

/* Draws the instances; the caller frees the scratch. */
static uint16_t*
draw(const r3d_instance_t* instances, int count, void** scratch) {
    raster_t raster = {.instances = instances, .instance_count = count, .width = SIZE, .height = SIZE, .clear = CLEAR};
    *scratch = malloc(raster_scratch_bytes(&raster));
    TEST_ASSERT_NOT_NULL(*scratch);
    raster.scratch = *scratch;
    raster_draw(&raster, &CAMERA, 0);
    return r3d_pipeline_carve(&raster).color;
}

/* A placement that scales every axis by `scale` and moves by (x, 0, z). */
static r3d_placement_t
placed(float x, float z, float scale) {
    return (r3d_placement_t){{{scale, 0, 0}, {0, scale, 0}, {0, 0, scale}}, {x, 0, z}};
}

/* The quad is 3.2 pixels from its centre in each direction at depth 10 and
 * a half field of view of 1, and a unit's x steps 3.2 pixels along the row. */
static uint16_t
pixel(const uint16_t* color, float units_from_center) {
    return color[(CENTER * SIZE) + CENTER + (int)(units_from_center * 3.2F)];
}

static void
test_a_mesh_with_no_placement_draws_where_it_is(void) {
    quad_t red;
    make_quad(&red, 255, 0, 0, 0);
    void* scratch;
    const r3d_instance_t one = {&red.mesh, NULL};
    const uint16_t* color = draw(&one, 1, &scratch);
    TEST_ASSERT_NOT_EQUAL_HEX16(CLEAR, pixel(color, 0.0F));
    TEST_ASSERT_EQUAL_HEX16(CLEAR, pixel(color, 3.0F));
    free(scratch);
}

static void
test_two_instances_appear_at_their_own_placements(void) {
    quad_t red;
    quad_t green;
    make_quad(&red, 255, 0, 0, 0);
    make_quad(&green, 0, 255, 0, 0);
    void* alone;
    const r3d_instance_t red_alone = {&red.mesh, NULL};
    const r3d_instance_t green_alone = {&green.mesh, NULL};
    const uint16_t red_pixel = pixel(draw(&red_alone, 1, &alone), 0.0F);
    free(alone);
    const uint16_t green_pixel = pixel(draw(&green_alone, 1, &alone), 0.0F);
    free(alone);
    TEST_ASSERT_NOT_EQUAL_HEX16(red_pixel, green_pixel);

    const r3d_placement_t left = placed(-3.0F, 0.0F, 1.0F);
    const r3d_placement_t right = placed(3.0F, 0.0F, 1.0F);
    const r3d_instance_t both[] = {{&red.mesh, &left}, {&green.mesh, &right}};
    void* scratch;
    const uint16_t* color = draw(both, 2, &scratch);
    TEST_ASSERT_EQUAL_HEX16(red_pixel, pixel(color, -3.0F));
    TEST_ASSERT_EQUAL_HEX16(green_pixel, pixel(color, 3.0F));
    TEST_ASSERT_EQUAL_HEX16(CLEAR, pixel(color, 0.0F));
    free(scratch);
}

static void
test_a_scale_widens_an_instance_about_its_position(void) {
    quad_t red;
    make_quad(&red, 255, 0, 0, 0);
    /* 1.5 units from the centre is outside the unit quad and inside a scale of 2. */
    const r3d_placement_t doubled_at = placed(0.0F, 0.0F, 2.0F);
    const r3d_instance_t plain = {&red.mesh, NULL};
    const r3d_instance_t doubled = {&red.mesh, &doubled_at};
    void* scratch;
    TEST_ASSERT_EQUAL_HEX16(CLEAR, pixel(draw(&plain, 1, &scratch), 1.5F));
    free(scratch);
    TEST_ASSERT_NOT_EQUAL_HEX16(CLEAR, pixel(draw(&doubled, 1, &scratch), 1.5F));
    free(scratch);
}

static void
test_a_rotation_turns_an_instance_about_its_position(void) {
    quad_t offset; /* centred 3 units to the right in its own space */
    make_quad(&offset, 0, 0, 255, 3);
    /* A half turn about y, baked: x and z negated. */
    const r3d_placement_t half_turn = {{{-1, 0, 0}, {0, 1, 0}, {0, 0, -1}}, {0, 0, 0}};
    const r3d_instance_t as_built = {&offset.mesh, NULL};
    const r3d_instance_t turned = {&offset.mesh, &half_turn};
    void* scratch;
    const uint16_t* color = draw(&as_built, 1, &scratch);
    TEST_ASSERT_NOT_EQUAL_HEX16(CLEAR, pixel(color, 3.0F));
    TEST_ASSERT_EQUAL_HEX16(CLEAR, pixel(color, -3.0F));
    free(scratch);
    color = draw(&turned, 1, &scratch);
    TEST_ASSERT_EQUAL_HEX16(CLEAR, pixel(color, 3.0F));
    TEST_ASSERT_NOT_EQUAL_HEX16(CLEAR, pixel(color, -3.0F));
    free(scratch);
}

static void
test_a_nearer_instance_covers_a_farther_one_whichever_is_drawn_first(void) {
    quad_t red;
    quad_t green;
    make_quad(&red, 255, 0, 0, 0);
    make_quad(&green, 0, 255, 0, 0);
    const r3d_placement_t nearer = placed(0.0F, 2.0F, 1.0F);
    const r3d_instance_t near_first[] = {{&green.mesh, &nearer}, {&red.mesh, NULL}};
    const r3d_instance_t far_first[] = {{&red.mesh, NULL}, {&green.mesh, &nearer}};
    void* scratch;
    const uint16_t first = pixel(draw(near_first, 2, &scratch), 0.0F);
    free(scratch);
    const uint16_t second = pixel(draw(far_first, 2, &scratch), 0.0F);
    free(scratch);
    TEST_ASSERT_EQUAL_HEX16(first, second);
    TEST_ASSERT_NOT_EQUAL_HEX16(CLEAR, first);
}

static void
test_the_camera_of_a_baked_placement_looks_down_its_third_column(void) {
    const r3d_placement_t turned = {{{0, 0, 1}, {0, 1, 0}, {-1, 0, 0}}, {5, 6, 7}}; /* a quarter turn about y */
    const r3d_scene_camera_t camera = {1.0F, 1.0F, &turned, NULL};
    vec3_t eye;
    vec3_t forward;
    r3d_scene_camera_sample(&camera, 0, &eye, &forward);
    TEST_ASSERT_EQUAL_FLOAT(5.0F, eye.x);
    TEST_ASSERT_EQUAL_FLOAT(-1.0F, forward.x);
    TEST_ASSERT_EQUAL_FLOAT(0.0F, forward.z);
    const r3d_scene_camera_t fixed = {1.0F, 1.0F, NULL, NULL};
    r3d_scene_camera_sample(&fixed, 0, &eye, &forward);
    TEST_ASSERT_EQUAL_FLOAT(-1.0F, forward.z);
}

void
suite_r3d_scene(void) {
    RUN_TEST(test_a_mesh_with_no_placement_draws_where_it_is);
    RUN_TEST(test_two_instances_appear_at_their_own_placements);
    RUN_TEST(test_a_scale_widens_an_instance_about_its_position);
    RUN_TEST(test_a_rotation_turns_an_instance_about_its_position);
    RUN_TEST(test_a_nearer_instance_covers_a_farther_one_whichever_is_drawn_first);
    RUN_TEST(test_the_camera_of_a_baked_placement_looks_down_its_third_column);
}

SUITE_REGISTER(suite_r3d_scene);
