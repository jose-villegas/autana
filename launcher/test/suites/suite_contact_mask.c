#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "render/contact/contact_mask.h"
#include "suites.h"
#include "unity.h"

static void
test_mask_without_triangles(void) {
    static const int16_t positions[][3] = {{-1, 1, -1}, {1, 1, -1}, {1, 1, 1}, {-1, 1, 1}};
    static const uint8_t records[][R3D_SKIN_INFLUENCES_MIN * 2] = {{0, 0, R3D_SKIN_WEIGHT_SUM, 0},
                                                                   {0, 0, R3D_SKIN_WEIGHT_SUM, 0},
                                                                   {0, 0, R3D_SKIN_WEIGHT_SUM, 0},
                                                                   {0, 0, R3D_SKIN_WEIGHT_SUM, 0}};
    const r3d_lit_mesh_t mesh = {.positions = positions, .vertex_count = 4, .position_scale = 1};
    const r3d_skin_t skin = {.vertices = &records[0][0],
                             .joint_count = 1,
                             .influences = R3D_SKIN_INFLUENCES_MIN,
                             .vertex_stride = sizeof records[0]};
    contact_joint_t joint;
    contact_mask_bind(&joint, &mesh, &skin);
    TEST_ASSERT_EQUAL_FLOAT(0, joint.centre.x);
    TEST_ASSERT_EQUAL_FLOAT(1, joint.centre.y);
    TEST_ASSERT_EQUAL_FLOAT(CONTACT_MASK_SPREAD, joint.radius.x);
    TEST_ASSERT_EQUAL_FLOAT(0, joint.radius.y);
    contact_mask_t mask = {.coverage = malloc(CONTACT_MASK_BYTES)};
    TEST_ASSERT_NOT_NULL(mask.coverage);
    const box3f_t box = {{-4, 0, -4}, {4, 4, 4}};
    mat4f_t world = mat4f_identity(), palette = mat4f_identity();
    contact_mask_build(&mask, &joint, 1, &palette, mesh.position_scale, &world, box);
    const int middle = CONTACT_MASK_SIDE / 2;
    const int at = middle * CONTACT_MASK_SIDE + middle;
    TEST_ASSERT_EQUAL_UINT8(UINT8_MAX, mask.coverage[at]);
    TEST_ASSERT_EQUAL_UINT8(0, mask.coverage[0]);
    uint8_t* saved = malloc(CONTACT_MASK_BYTES);
    TEST_ASSERT_NOT_NULL(saved);
    memcpy(saved, mask.coverage, CONTACT_MASK_BYTES);
    palette.m[1][3] = 4;
    contact_mask_build(&mask, &joint, 1, &palette, mesh.position_scale, &world, box);
    TEST_ASSERT_EQUAL_MEMORY(saved, mask.coverage, CONTACT_MASK_BYTES);
    palette.m[0][3] = 2;
    contact_mask_build(&mask, &joint, 1, &palette, mesh.position_scale, &world, box);
    TEST_ASSERT_EQUAL_UINT8(0, mask.coverage[at - middle / 2]);
    TEST_ASSERT_EQUAL_UINT8(UINT8_MAX, mask.coverage[at + middle / 2]);
    free(saved);
    free(mask.coverage);
}

static void
test_mask_weighted_spread(void) {
    static const int16_t positions[][3] = {{0, 0, 0}, {4, 0, 0}};
    static const uint8_t records[][R3D_SKIN_INFLUENCES_MIN * 2] = {
        {0, 1, R3D_SKIN_WEIGHT_SUM, 0}, {0, 1, R3D_SKIN_WEIGHT_SUM / 3, 2 * R3D_SKIN_WEIGHT_SUM / 3}};
    const r3d_lit_mesh_t mesh = {.positions = positions, .vertex_count = 2};
    const r3d_skin_t skin = {.vertices = &records[0][0],
                             .joint_count = 3,
                             .influences = R3D_SKIN_INFLUENCES_MIN,
                             .vertex_stride = sizeof records[0]};
    contact_joint_t joints[3];
    contact_mask_bind(joints, &mesh, &skin);
    TEST_ASSERT_EQUAL_FLOAT(1, joints[0].centre.x);
    TEST_ASSERT_EQUAL_FLOAT(4, joints[1].centre.x);
    TEST_ASSERT_EQUAL_FLOAT(CONTACT_MASK_SPREAD * sqrtf(3), joints[0].radius.x);
    TEST_ASSERT_EQUAL_FLOAT(0, joints[1].radius.x);
    TEST_ASSERT_EQUAL_FLOAT(0, joints[2].weight);
}

static void
test_mask_palette_scale_and_world_transform(void) {
    const contact_joint_t joint = {.centre = {8, 0, 0}, .radius = {4, 0, 1}, .weight = 1};
    const int scale = 4;
    mat4f_t palette = mat4f_identity(), world = mat4f_identity();
    palette.m[0][3] = scale;
    world.m[0][0] = 0;
    world.m[0][2] = 2;
    world.m[2][0] = -2;
    world.m[2][2] = 0;
    world.m[0][3] = 3;
    const box3f_t box = {{-1, 0, -10}, {7, 4, -2}};
    contact_mask_t mask = {.coverage = malloc(CONTACT_MASK_BYTES)};
    TEST_ASSERT_NOT_NULL(mask.coverage);
    contact_mask_build(&mask, &joint, 1, &palette, scale, &world, box);
    const int middle = CONTACT_MASK_SIDE / 2;
    const int at = middle * CONTACT_MASK_SIDE + middle;
    TEST_ASSERT_EQUAL_UINT8(UINT8_MAX, mask.coverage[at]);
    TEST_ASSERT_GREATER_THAN_UINT8(0, mask.coverage[at + (CONTACT_MASK_SIDE / 8) * CONTACT_MASK_SIDE]);
    TEST_ASSERT_EQUAL_UINT8(0, mask.coverage[at + CONTACT_MASK_SIDE / 8]);
    contact_mask_build(&mask, &joint, 1, &palette, scale, &world, (box3f_t){0});
    for (int i = 0; i < CONTACT_MASK_BYTES; i++) {
        TEST_ASSERT_EQUAL_UINT8(0, mask.coverage[i]);
    }
    free(mask.coverage);
}

void
suite_contact_mask(void) {
    RUN_TEST(test_mask_without_triangles);
    RUN_TEST(test_mask_weighted_spread);
    RUN_TEST(test_mask_palette_scale_and_world_transform);
}

SUITE_REGISTER(suite_contact_mask);
