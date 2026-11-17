#include <stdlib.h>
#include "render/contact/contact_mask.h"
#include "suites.h"
#include "unity.h"

static void
test_mask_plain_silhouette(void) {
    static const int16_t positions[][3] = {{-1, 1, -1}, {1, 1, -1}, {1, 1, 1}, {-1, 1, 1}};
    static const uint16_t triangles[][3] = {{0, 1, 2}, {0, 2, 3}};
    static const r3d_lit_cluster_t cluster = {.vertex_count = 4, .triangle_count = 2};
    const r3d_lit_mesh_t mesh = {
        .positions = positions, .triangles = triangles, .clusters = &cluster, .cluster_count = 1, .position_scale = 1};
    contact_mask_t mask = {.coverage = malloc(CONTACT_MASK_BYTES), .side = CONTACT_MASK_SIDE};
    TEST_ASSERT_NOT_NULL(mask.coverage);
    const box3f_t box = {{-2, 0, -2}, {2, 4, 2}};
    mat4f_t world = mat4f_identity();
    contact_mask_build(&mask, &mesh, &world, box, 0);
    const int middle = CONTACT_MASK_SIDE / 2;
    const int at = middle * CONTACT_MASK_SIDE + middle;
    TEST_ASSERT_EQUAL_INT(UINT8_MAX, mask.coverage[at]);
    TEST_ASSERT_EQUAL_INT(0, mask.coverage[0]);
    TEST_ASSERT_GREATER_THAN_INT(0, mask.coverage[at - middle / 2]);
    TEST_ASSERT_EQUAL_INT(0, mask.coverage[at - middle + 1]);
    const int low = CONTACT_MASK_SIDE / 4, high = CONTACT_MASK_SIDE - low;
    for (int y = 0; y < CONTACT_MASK_SIDE; y++) {
        for (int x = 0; x < CONTACT_MASK_SIDE; x++) {
            const uint8_t cell = mask.coverage[y * CONTACT_MASK_SIDE + x];
            if (x > low && x < high - 1 && y > low && y < high - 1) {
                TEST_ASSERT_EQUAL_UINT8(UINT8_MAX, cell);
            }
            if (x < low - 1 || x > high || y < low - 1 || y > high) {
                TEST_ASSERT_EQUAL_UINT8(0, cell);
            }
        }
    }
    world.m[1][3] = 1;
    contact_mask_build(&mask, &mesh, &world, box, 0);
    TEST_ASSERT_EQUAL_INT(UINT8_MAX, mask.coverage[at]);
    world.m[1][3] = 4;
    contact_mask_build(&mask, &mesh, &world, box, 0);
    TEST_ASSERT_EQUAL_INT(UINT8_MAX, mask.coverage[at]);
    free(mask.coverage);
}

void
suite_contact_mask(void) {
    RUN_TEST(test_mask_plain_silhouette);
}

SUITE_REGISTER(suite_contact_mask);
