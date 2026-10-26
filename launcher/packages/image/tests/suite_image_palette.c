#include <stdint.h>
#include <stdlib.h>

#include "testkit/suite.h"
#include "unity.h"

#include "image/palette/palette.h"
#include "image/pixel/rgb565.h"

static void
test_palette_index_uses_native_rgb565_keys(void) {
    static const rgb565be_t entries[] = {RGB565BE(0xFF0000), RGB565BE(0x00FF00), RGB565BE(0x0000FF)};
    const palette_t palette = {.name = "primaries", .entries = entries, .count = sizeof entries / sizeof entries[0]};
    uint8_t* map = calloc((size_t)UINT16_MAX + 1, sizeof *map);
    TEST_ASSERT_NOT_NULL(map);
    map[RGB565(0xFF0000)] = 1;
    map[RGB565(0x00FF00)] = 2;
    map[RGB565(0x0000FF)] = 3;
    int indices[sizeof entries / sizeof entries[0]];
    for (int i = 0; i < palette.count; i++) {
        indices[i] = palette_index_of(palette.entries[i], map);
    }
    free(map);
    for (int i = 0; i < palette.count; i++) {
        TEST_ASSERT_EQUAL_INT(i + 1, indices[i]);
    }
}

void
run_image_palette_suite(void) {
    RUN_TEST(test_palette_index_uses_native_rgb565_keys);
}

SUITE_REGISTER(run_image_palette_suite);
