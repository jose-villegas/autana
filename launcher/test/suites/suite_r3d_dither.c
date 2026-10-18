/* Walked colour quantisation keeps coverage and depth independent of the palette. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfx/target/gfx_color.h"
#include "render/r3d_span_internal.h"
#include "services/tune.h"
#include "suites.h"
#include "unity.h"

#if TUNE_HOT_LIVE && TUNE_ENABLED
#define WIDTH         64
#define HEIGHT        64
#define PIXELS        (WIDTH * HEIGHT)
#define CHANNEL_MAX   255
#define CHANNEL_SCALE 256
#define MAX_STEPS     8

static uint16_t *colors, *depths, *reference_colors, *reference_depths;
static bool tune_ok;

static void
reply(const char* line) {
    tune_ok = strncmp(line, "TUNE_OK", strlen("TUNE_OK")) == 0;
}

static void
release_fixture(void) {
    (void)tune_handle_line("RESET r3d_span.blend_steps", reply);
    (void)tune_handle_line("RESET r3d_span.dither_cell_bits", reply);
    free(colors);
    free(depths);
    free(reference_colors);
    free(reference_depths);
    colors = depths = reference_colors = reference_depths = NULL;
}

static void
set(const char* line) {
    tune_ok = false;
    TEST_ASSERT_TRUE(tune_handle_line(line, reply));
    TEST_ASSERT_TRUE(tune_ok);
}

static r3d_span_target_t
fixture(void) {
    suite_set_test_cleanup(release_fixture);
    colors = calloc(PIXELS, sizeof(*colors));
    depths = calloc(PIXELS, sizeof(*depths));
    reference_colors = calloc(PIXELS, sizeof(*reference_colors));
    reference_depths = calloc(PIXELS, sizeof(*reference_depths));
    TEST_ASSERT_NOT_NULL(colors);
    TEST_ASSERT_NOT_NULL(depths);
    TEST_ASSERT_NOT_NULL(reference_colors);
    TEST_ASSERT_NOT_NULL(reference_depths);
    return r3d_span_target(colors, depths, WIDTH, 0, HEIGHT);
}

static void
render(const r3d_span_target_t* target, const r3d_span_vertex_t vertices[3]) {
    memset(colors, 0, PIXELS * sizeof(*colors));
    memset(depths, 0, PIXELS * sizeof(*depths));
    r3d_span_triangle(target, vertices, vertices + 1, vertices + 2);
}

static void
test_walked_dither_palette_and_depth(void) {
    const r3d_span_target_t target = fixture();
    /* The clipping forces the scalar span clamp as well as the depth clamp. */
    const r3d_span_vertex_t vertices[3] = {
        {-16 * R3D_SUBPIXEL, -8 * R3D_SUBPIXEL, 0.25F, 0, 0, 0},
        {80 * R3D_SUBPIXEL, 0, 0.75F, CHANNEL_MAX, CHANNEL_MAX, CHANNEL_MAX},
        {0, 80 * R3D_SUBPIXEL, 0.5F, 96, 96, 96},
    };
    render(&target, vertices);
    memcpy(reference_colors, colors, PIXELS * sizeof(*colors));
    memcpy(reference_depths, depths, PIXELS * sizeof(*depths));
    set("SET r3d_span.blend_steps 0");
    render(&target, vertices);
    TEST_ASSERT_EQUAL_UINT16_ARRAY(reference_colors, colors, PIXELS);
    for (int steps = 1; steps <= MAX_STEPS; steps++) {
        char command[TUNE_NAME_MAX + 16];
        (void)snprintf(command, sizeof(command), "SET r3d_span.blend_steps %d", steps);
        set(command);
        uint16_t palette[MAX_STEPS + 1];
        for (int k = 0; k <= steps; k++) {
            const int channel = (int)((float)CHANNEL_MAX * ((float)k / (float)steps) * CHANNEL_SCALE);
            palette[k] = r3d_span_pack(channel, channel, channel);
        }
        for (int bits = 1; bits <= 3; bits++) {
            (void)snprintf(command, sizeof(command), "SET r3d_span.dither_cell_bits %d", bits);
            set(command);
            render(&target, vertices);
            TEST_ASSERT_EQUAL_UINT16_ARRAY(reference_depths, depths, PIXELS);
            for (int i = 0; i < PIXELS; i++) {
                if (depths[i] != 0) {
                    bool found = false;
                    for (int k = 0; k <= steps; k++) {
                        found |= colors[i] == palette[k];
                    }
                    TEST_ASSERT_TRUE(found);
                }
            }
        }
    }
    set("RESET r3d_span.blend_steps");
    render(&target, vertices);
    TEST_ASSERT_EQUAL_UINT16_ARRAY(reference_colors, colors, PIXELS);
    release_fixture();
}

static void
test_walked_dither_solid_and_pattern(void) {
    const r3d_span_target_t target = fixture();
    r3d_span_vertex_t vertices[3] = {
        {0, 0, 0.4F, 100, 100, 100},
        {WIDTH * R3D_SUBPIXEL, 0, 0.6F, 101, 101, 101},
        {0, HEIGHT * R3D_SUBPIXEL, 0.8F, 102, 102, 102},
    };
    render(&target, vertices);
    memcpy(reference_depths, depths, PIXELS * sizeof(*depths));
    set("SET r3d_span.blend_steps 1");
    render(&target, vertices);
    const uint16_t solid = r3d_span_pack(100 * CHANNEL_SCALE, 100 * CHANNEL_SCALE, 100 * CHANNEL_SCALE);
    TEST_ASSERT_EQUAL_UINT16_ARRAY(reference_depths, depths, PIXELS);
    for (int i = 0; i < PIXELS; i++) {
        if (depths[i] != 0) {
            TEST_ASSERT_EQUAL_HEX16(solid, colors[i]);
        }
    }
    vertices[0].r = vertices[0].g = vertices[0].b = 0;
    vertices[1].r = vertices[1].g = vertices[1].b = CHANNEL_MAX;
    vertices[2].r = vertices[2].g = vertices[2].b = 0;
    const uint16_t white =
        r3d_span_pack(CHANNEL_MAX * CHANNEL_SCALE, CHANNEL_MAX * CHANNEL_SCALE, CHANNEL_MAX * CHANNEL_SCALE);
    for (int bits = 1; bits <= 3; bits++) {
        char command[TUNE_NAME_MAX + 16];
        (void)snprintf(command, sizeof(command), "SET r3d_span.dither_cell_bits %d", bits);
        set(command);
        render(&target, vertices);
        TEST_ASSERT_EQUAL_UINT16_ARRAY(reference_depths, depths, PIXELS);
        int bright = 0;
        float expected = 0;
        const int region = WIDTH / 4;
        for (int y = 0; y < region; y++) {
            for (int x = 0; x < region; x++) {
                const int t = (int)(((float)x + 0.5F) / WIDTH * CHANNEL_SCALE);
                const int threshold = (2 * gfx_dither_rank(x, y, bits) + 1) << (7 - 2 * bits);
                TEST_ASSERT_EQUAL_HEX16(t + threshold >= CHANNEL_SCALE ? white : 0, colors[y * WIDTH + x]);
                bright += colors[y * WIDTH + x] == white;
                expected += ((float)x + 0.5F) / WIDTH;
            }
        }
        TEST_ASSERT_FLOAT_WITHIN((float)(region * region) / (float)(1 << (2 * bits)), expected, (float)bright);
        const r3d_dither_ranks_t* pattern = r3d_dither_ranks();
        TEST_ASSERT_EQUAL_INT(1 << (2 * bits), pattern->levels);
        for (int y = 0; y < R3D_DITHER_SIDE; y++) {
            for (int x = 0; x < R3D_DITHER_SIDE; x++) {
                TEST_ASSERT_EQUAL_INT(gfx_dither_rank(x, y, bits), pattern->ranks[y][x]);
            }
        }
    }
    release_fixture();
}
#endif

void
run_r3d_dither_suite(void) {
#if TUNE_HOT_LIVE && TUNE_ENABLED
    RUN_TEST(test_walked_dither_palette_and_depth);
    RUN_TEST(test_walked_dither_solid_and_pattern);
#endif
}

SUITE_REGISTER(run_r3d_dither_suite);
