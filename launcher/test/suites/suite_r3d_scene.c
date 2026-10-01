/*
 * Portable suite: a raster drawing several mesh instances, each at its own
 * transform, into one picture. Each instance is one unit quad facing the
 * camera, so where a transform put it is read straight off the picture:
 * a pixel is the quad's colour, or the clear colour.
 */

#include <stdint.h>
#include <stdlib.h>

#include "suites.h"
#include "unity.h"

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
    q->cluster = (r3d_lit_cluster_t){0, 4, 0, 2, {(int16_t)(cx - 1), -1, 0}, {(int16_t)(cx + 1), 1, 0}, true};
    q->node = (r3d_lit_node_t){{(int16_t)(cx - 1), -1, 0}, {(int16_t)(cx + 1), 1, 0}, 0, 1, true};
    q->mesh = (r3d_lit_mesh_t){q->positions, q->colors, q->triangles, &q->cluster, &q->node, 4, 2, 1, 1, 1, NULL};
}

static const camera_t CAMERA = {{0.0F, 0.0F, 10.0F}, {0.0F, 0.0F, -1.0F}, 1.0F, 1.0F};

/* Draws the instances; the caller frees the scratch. */
static uint16_t*
draw(const raster_instance_t* instances, int count, void** scratch) {
    raster_t raster = {.instances = instances, .instance_count = count, .width = SIZE, .height = SIZE, .clear = CLEAR};
    *scratch = malloc(raster_scratch_bytes(&raster));
    TEST_ASSERT_NOT_NULL(*scratch);
    raster.scratch = *scratch;
    raster_draw(&raster, &CAMERA, 0);
    return r3d_pipeline_carve(&raster).color;
}

static const r3d_transform_t IDENTITY = {{0, 0, 0}, {0, 0, 0}, {1, 1, 1}};

static r3d_transform_t
moved(float x) {
    return (r3d_transform_t){{x, 0, 0}, {0, 0, 0}, {1, 1, 1}};
}

/* The quad is 3.2 pixels from its centre in each direction at depth 10 and
 * a half field of view of 1, and a unit's x steps 3.2 pixels along the row. */
static uint16_t
pixel(const uint16_t* color, float units_from_center) {
    return color[(CENTER * SIZE) + CENTER + (int)(units_from_center * 3.2F)];
}

static void
test_an_untransformed_instance_draws_where_its_mesh_is(void) {
    quad_t red;
    make_quad(&red, 255, 0, 0, 0);
    void* scratch;
    const uint16_t* color = draw(&(raster_instance_t){&red.mesh, IDENTITY}, 1, &scratch);
    TEST_ASSERT_NOT_EQUAL_HEX16(CLEAR, pixel(color, 0.0F));
    TEST_ASSERT_EQUAL_HEX16(CLEAR, pixel(color, 3.0F));
    free(scratch);
}

static void
test_two_instances_appear_at_their_own_transforms(void) {
    quad_t red, green;
    make_quad(&red, 255, 0, 0, 0);
    make_quad(&green, 0, 255, 0, 0);
    void* alone;
    const uint16_t red_pixel = pixel(draw(&(raster_instance_t){&red.mesh, IDENTITY}, 1, &alone), 0.0F);
    free(alone);
    const uint16_t green_pixel = pixel(draw(&(raster_instance_t){&green.mesh, IDENTITY}, 1, &alone), 0.0F);
    free(alone);
    TEST_ASSERT_NOT_EQUAL_HEX16(red_pixel, green_pixel);

    const raster_instance_t both[] = {{&red.mesh, moved(-3.0F)}, {&green.mesh, moved(3.0F)}};
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
    const raster_instance_t plain = {&red.mesh, IDENTITY};
    const raster_instance_t doubled = {&red.mesh, {{0, 0, 0}, {0, 0, 0}, {2, 2, 2}}};
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
    const raster_instance_t as_built = {&offset.mesh, IDENTITY};
    const raster_instance_t turned = {&offset.mesh, {{0, 0, 0}, {0, 180, 0}, {1, 1, 1}}};
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
    quad_t red, green;
    make_quad(&red, 255, 0, 0, 0);
    make_quad(&green, 0, 255, 0, 0);
    const raster_instance_t near_first[] = {{&green.mesh, {{0, 0, 2}, {0, 0, 0}, {1, 1, 1}}}, {&red.mesh, IDENTITY}};
    const raster_instance_t far_first[] = {{&red.mesh, IDENTITY}, {&green.mesh, {{0, 0, 2}, {0, 0, 0}, {1, 1, 1}}}};
    void* scratch;
    const uint16_t first = pixel(draw(near_first, 2, &scratch), 0.0F);
    free(scratch);
    const uint16_t second = pixel(draw(far_first, 2, &scratch), 0.0F);
    free(scratch);
    TEST_ASSERT_EQUAL_HEX16(first, second);
}

void
suite_r3d_scene(void) {
    RUN_TEST(test_an_untransformed_instance_draws_where_its_mesh_is);
    RUN_TEST(test_two_instances_appear_at_their_own_transforms);
    RUN_TEST(test_a_scale_widens_an_instance_about_its_position);
    RUN_TEST(test_a_rotation_turns_an_instance_about_its_position);
    RUN_TEST(test_a_nearer_instance_covers_a_farther_one_whichever_is_drawn_first);
}

SUITE_REGISTER(suite_r3d_scene);
