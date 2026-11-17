#include <stdlib.h>
#include "render/contact/contact_mask.h"
#include "suites.h"
#include "unity.h"

static void
test_mask_height_footprint_and_empty(void) {
    static const int16_t positions[][3] = {{-1, 1, -1}, {1, 1, -1}, {1, 1, 1}, {-1, 1, 1},
                                           {-1, 3, -1}, {1, 3, -1}, {1, 3, 1}, {-1, 3, 1}};
    static const uint16_t triangles[][3] = {{0, 1, 2}, {0, 2, 3}, {4, 5, 6}, {4, 6, 7}};
    static const r3d_lit_cluster_t cluster = {.vertex_count = 8, .triangle_count = 4};
    const r3d_lit_mesh_t mesh = {
        .positions = positions, .triangles = triangles, .clusters = &cluster, .cluster_count = 1, .position_scale = 1};
    contact_mask_t mask = {.coverage = malloc(CONTACT_MASK_BYTES), .side = CONTACT_MASK_SIDE};
    TEST_ASSERT_NOT_NULL(mask.coverage);
    const box3f_t box = {{-2, 0, -2}, {2, 4, 2}};
    mat4f_t world = mat4f_identity();
    contact_mask_build(&mask, &mesh, world, box, 0, 4, 1);
    const int middle = CONTACT_MASK_SIDE / 2;
    const int at = middle * CONTACT_MASK_SIDE + middle;
    TEST_ASSERT_INT_WITHIN(1, UINT8_MAX * 9 / 16, mask.coverage[at]);
    TEST_ASSERT_EQUAL_INT(0, mask.coverage[0]);
    TEST_ASSERT_GREATER_THAN_INT(0, mask.coverage[at - middle / 2]);
    TEST_ASSERT_EQUAL_INT(0, mask.coverage[at - middle + 1]);
    world.m[1][3] = 1;
    contact_mask_build(&mask, &mesh, world, box, 0, 4, 1);
    TEST_ASSERT_INT_WITHIN(1, UINT8_MAX / 4, mask.coverage[at]);
    world.m[1][3] = 4;
    contact_mask_build(&mask, &mesh, world, box, 0, 4, 1);
    for (int i = 0; i < CONTACT_MASK_BYTES; i++) {
        TEST_ASSERT_EQUAL_INT(0, mask.coverage[i]);
    }
    free(mask.coverage);
}

void
suite_contact_mask(void) {
    RUN_TEST(test_mask_height_footprint_and_empty);
}

SUITE_REGISTER(suite_contact_mask);
