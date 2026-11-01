#include "suites.h"
#include "unity.h"

#include "render/raster_internal.h"

#define POSITION_COUNT 32
#define RANGE_ITEMS    3
#define LOW_FIRST      2
#define LOW_COUNT      4
#define HIGH_FIRST     20
#define HIGH_COUNT     5
#define MIDDLE_FIRST   10
#define MIDDLE_COUNT   3

static void
test_transform_range_covers_every_position_array(void) {
    const int16_t positions[POSITION_COUNT][3] = {0};
    const r3d_lit_mesh_t meshes[RANGE_ITEMS] = {
        {.positions = positions + HIGH_FIRST, .vertex_count = HIGH_COUNT},
        {.positions = positions + LOW_FIRST, .vertex_count = LOW_COUNT},
        {.positions = positions + MIDDLE_FIRST, .vertex_count = MIDDLE_COUNT},
    };
    const r3d_pipeline_part_t parts[RANGE_ITEMS] = {
        {.mesh = &meshes[0]},
        {.mesh = &meshes[1]},
        {.mesh = &meshes[2]},
    };
    const void* addr;
    size_t size;
    raster_transform_range(parts, RANGE_ITEMS, &addr, &size);
    TEST_ASSERT_EQUAL_PTR(positions + LOW_FIRST, addr);
    TEST_ASSERT_EQUAL_size_t((HIGH_FIRST + HIGH_COUNT - LOW_FIRST) * sizeof positions[0], size);
}

void
run_raster_autoload_suite(void) {
    RUN_TEST(test_transform_range_covers_every_position_array);
}

SUITE_REGISTER(run_raster_autoload_suite);
