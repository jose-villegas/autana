/*
 * Portable suite: a raster drawing several mesh instances, each at its own
 * baked placement, into one picture. Each instance is one unit quad facing the
 * camera, so where a transform put it is read straight off the picture:
 * a pixel is the quad's colour, or the clear colour.
 */

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "render_view_fixture.h"
#include "suites.h"
#include "unity.h"

#include "r3d_quad_mesh.h"
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
    const render_view_t frame_view = render_view_fixture(&CAMERA, &raster, 0);
    raster_draw(&raster, &frame_view);
    return raster_color(&raster);
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
    for (int axis = 0; axis < 3; axis++) {
        ticks.cluster.lo[axis] *= POSITION_SCALE;
        ticks.cluster.hi[axis] *= POSITION_SCALE;
        ticks.node.lo[axis] *= POSITION_SCALE;
        ticks.node.hi[axis] *= POSITION_SCALE;
    }
    ticks.mesh.position_scale = POSITION_SCALE;
    const r3d_placement_t placement = placed(3.0F, 0.0F, 1.0F);
    const r3d_instance_t reference = {&unit.mesh, &placement}, scaled = {&ticks.mesh, &placement};
    void* reference_scratch;
    void* scaled_scratch;
    const uint16_t* expected = draw(&reference, 1, &reference_scratch);
    const uint16_t* actual = draw(&scaled, 1, &scaled_scratch);
    TEST_ASSERT_NOT_EQUAL_HEX16(CLEAR, pixel(actual, 3.0F));
    TEST_ASSERT_EQUAL_HEX16(CLEAR, pixel(actual, 0.0F));
    TEST_ASSERT_EQUAL_HEX16_ARRAY(expected, actual, SIZE * SIZE);
    free(reference_scratch);
    free(scaled_scratch);
}

static void
test_every_lens_operation_keeps_the_affine_bottom_row(void) {
    const r3d_placement_t placement = placed(3.0F, 2.0F, 0.5F);
    for (int quarter = 0; quarter < 4; quarter++) {
        r3d_lens_t lens;
        memset(&lens, 0x5a, sizeof lens);
        const render_view_t frame_view = render_view_fixture_at(&CAMERA, (viewport_t){SIZE, SIZE, quarter});
        r3d_lens_init(&lens, &frame_view, 8);
        for (int operation = 0; operation < 3; operation++) {
            if (operation == 1) {
                r3d_lens_fit(&lens, SIZE / 2, SIZE);
            } else if (operation == 2) {
                r3d_lens_place(&lens, &placement, 8);
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
    vec3f_t eye;
    vec3f_t forward;
    r3d_scene_camera_sample(&camera, 0, &eye, &forward);
    TEST_ASSERT_EQUAL_FLOAT(5.0F, eye.x);
    TEST_ASSERT_EQUAL_FLOAT(-1.0F, forward.x);
    TEST_ASSERT_EQUAL_FLOAT(0.0F, forward.z);
    const r3d_scene_camera_t fixed = {1.0F, 1.0F, NULL, NULL};
    r3d_scene_camera_sample(&fixed, 0, &eye, &forward);
    TEST_ASSERT_EQUAL_FLOAT(-1.0F, forward.z);
}

static void
test_a_placement_from_a_pose_stands_the_camera_there_looking_down_its_plus_z(void) {
    transformf_t pose = TRANSFORMF_IDENTITY;
    transformf_set_position(&pose, (vec3f_t){3.0F, 2.0F, 4.0F});
    transformf_look_at(&pose, (vec3f_t){0.0F, 1.0F, 0.0F}, (vec3f_t){0.0F, 1.0F, 0.0F});
    const r3d_placement_t placement = r3d_scene_camera_placement(&pose);
    const r3d_scene_camera_t camera = {1.0F, 1.0F, &placement, NULL};
    vec3f_t eye;
    vec3f_t forward;
    r3d_scene_camera_sample(&camera, 0, &eye, &forward);
    const vec3f_t ahead = quatf_rotate(pose.rotation, (vec3f_t){0.0F, 0.0F, 1.0F});
    TEST_ASSERT_TRUE(vec3f_equal(pose.position, eye));
    TEST_ASSERT_FLOAT_WITHIN(1.0E-6F, ahead.x, forward.x);
    TEST_ASSERT_FLOAT_WITHIN(1.0E-6F, ahead.y, forward.y);
    TEST_ASSERT_FLOAT_WITHIN(1.0E-6F, ahead.z, forward.z);
}

static void
test_a_mesh_s_bounding_sphere_is_centred_on_its_box_and_reaches_its_furthest_vertex(void) {
    static const int16_t corners[4][3] = {{0, 0, 0}, {8, 0, 0}, {8, 4, 0}, {0, 2, 0}};
    static const r3d_lit_cluster_t cluster = {0, 4, 0, 2, {0, 0, 0}, {8, 4, 0}, false};
    static const r3d_lit_node_t node = {{0, 0, 0}, {8, 4, 0}, 0, 1, true};
    r3d_lit_mesh_t mesh = r3d_quad_mesh(corners, NULL, NULL, &cluster, &node);
    mesh.position_scale = 2; /* ticks per unit: the box is 4 by 2 units */
    vec3f_t centre;
    float radius;
    r3d_lit_mesh_bounding_sphere(&mesh, &centre, &radius);
    TEST_ASSERT_TRUE(vec3f_equal((vec3f_t){2.0F, 1.0F, 0.0F}, centre));
    TEST_ASSERT_FLOAT_WITHIN(1.0E-6F, sqrtf(5.0F), radius); /* corner (4, 2) or (0, 0): 2 by 1 from the centre */
}

void
suite_r3d_scene(void) {
    RUN_TEST(test_a_mesh_with_no_placement_draws_where_it_is);
    RUN_TEST(test_two_instances_appear_at_their_own_placements);
    RUN_TEST(test_position_ticks_keep_a_placed_instance_in_model_units);
    RUN_TEST(test_every_lens_operation_keeps_the_affine_bottom_row);
    RUN_TEST(test_a_scale_widens_an_instance_about_its_position);
    RUN_TEST(test_a_rotation_turns_an_instance_about_its_position);
    RUN_TEST(test_a_nearer_instance_covers_a_farther_one_whichever_is_drawn_first);
    RUN_TEST(test_the_camera_of_a_baked_placement_looks_down_its_third_column);
    RUN_TEST(test_a_placement_from_a_pose_stands_the_camera_there_looking_down_its_plus_z);
    RUN_TEST(test_a_mesh_s_bounding_sphere_is_centred_on_its_box_and_reaches_its_furthest_vertex);
}

SUITE_REGISTER(suite_r3d_scene);
