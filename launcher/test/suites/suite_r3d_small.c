#ifndef DEVICE_BUILD
#include "render/r3d_span.h"

/* The constant fill is the default-image oracle; this copy enables hot tunables. */
#define TUNE_HOT_LIVE           1
#define r3d_span_triangle       hot_triangle
#define r3d_span_triangle_solid hot_triangle_solid
#define r3d_span_triangle_write hot_triangle_write
#define r3d_span_hidden         hot_hidden
#define r3d_span_plane_bound    hot_plane_bound
#define r3d_span_plane_in_range hot_plane_in_range
#define r3d_span_stop_after     hot_stop_after
#include "render/r3d_span.c"
#undef r3d_span_triangle
#undef r3d_span_triangle_solid
#undef r3d_span_triangle_write
#include <stdlib.h>
#include <string.h>
#include "suites.h"
#include "unity.h"

#define TEST_SIDE   24
#define TEST_PIXELS (TEST_SIDE * TEST_SIDE)

static void
knob(const char* name, int value) {
    const tune_entry_t* entry = tune_find(tune_shared(), name);
    TEST_ASSERT_NOT_NULL(entry);
    TEST_ASSERT_TRUE(value >= entry->low && value <= entry->high);
    *entry->value = value;
}

static void
reset_knobs(void) {
    for (const tune_entry_t* e = tune_shared()->first; e != NULL; e = e->next) {
        if (strncmp(e->name, "r3d_span.", strlen("r3d_span.")) == 0) {
            *e->value = e->initial;
        }
    }
}

static uint16_t* pixels;

static void
cleanup(void) {
    reset_knobs();
    free(pixels);
    pixels = NULL;
}

static r3d_span_target_t
fixture(void) {
    if (pixels == NULL) {
        pixels = malloc(sizeof(*pixels) * TEST_PIXELS * 4);
    }
    suite_set_test_cleanup(cleanup);
    TEST_ASSERT_NOT_NULL(pixels);
    memset(pixels, 0, sizeof(*pixels) * TEST_PIXELS * 4);
    return r3d_span_target(pixels, pixels + TEST_PIXELS, TEST_SIDE, 0, TEST_SIDE);
}

static void
test_small_defaults_match_constant_fill(void) {
    uint32_t state = 937;
    for (int i = 0; i < 1000; i++) {
        r3d_span_vertex_t v[3];
        for (int k = 0; k < 3; k++) {
            state = state * 1664525u + 1013904223u;
            v[k] = (r3d_span_vertex_t){(int32_t)(state % (TEST_SIDE * R3D_SUBPIXEL)),
                                       (int32_t)((state >> 12) % (TEST_SIDE * R3D_SUBPIXEL)),
                                       0.2F + (float)(state & 255u) / 512.0F,
                                       (float)(state & 255u),
                                       (float)((state >> 8) & 255u),
                                       (float)((state >> 16) & 255u)};
        }
        const r3d_span_target_t hot = fixture();
        const r3d_span_target_t normal =
            r3d_span_target(pixels + TEST_PIXELS * 2, pixels + TEST_PIXELS * 3, TEST_SIDE, 0, TEST_SIDE);
        hot_triangle(&hot, &v[0], &v[1], &v[2]);
        r3d_span_triangle(&normal, &v[0], &v[1], &v[2]);
        TEST_ASSERT_EQUAL_MEMORY(pixels, pixels + TEST_PIXELS * 2, sizeof(*pixels) * TEST_PIXELS * 2);
    }
}

static void
test_small_spread_channel_boundaries_and_faces(void) {
    (void)fixture();
    knob("r3d_span.small_flat_max_side", SMALL_MAX_SIDE_TUNE);
    knob("r3d_span.small_flat_spread", 1);
    for (int channel = 0; channel < 3; channel++) {
        const float step =
            255.0F / (channel == 1 ? (float)gfx_rgb565_g6(UINT16_MAX) : (float)gfx_rgb565_r5(UINT16_MAX));
        for (int delta = -1; delta <= 1; delta++) {
            r3d_span_vertex_t v[3] = {{0, 0, 0.2F, 0, 0, 0},
                                      {SMALL_MAX_SIDE_TUNE * R3D_SUBPIXEL, 0, 0.8F, 0, 0, 0},
                                      {0, SMALL_MAX_SIDE_TUNE * R3D_SUBPIXEL, 0.5F, 0, 0, 0}};
            if (channel == 0) {
                v[1].r = step + (float)delta * 0.01F;
            }
            if (channel == 1) {
                v[1].g = step + (float)delta * 0.01F;
            }
            if (channel == 2) {
                v[1].b = step + (float)delta * 0.01F;
            }
            const r3d_span_target_t target = fixture();
            hot_triangle(&target, &v[0], &v[1], &v[2]);
            const bool flat = pixels[TEST_PIXELS + 1] == pixels[TEST_PIXELS + 10];
            TEST_ASSERT_EQUAL_INT(delta <= 0, flat);
            memset(pixels, 0, sizeof(*pixels) * TEST_PIXELS * 2);
            hot_triangle_solid(&target, &v[0], &v[1], &v[2], GFX_RGB(0x123456));
            TEST_ASSERT_EQUAL_UINT16(pixels[TEST_PIXELS + 1], pixels[TEST_PIXELS + 10]);
        }
    }
}

static void
test_small_dither_equal_colours_and_vertex_shares(void) {
    const r3d_span_target_t target = fixture();
    knob("r3d_span.small_max_side", SMALL_MAX_SIDE_TUNE);
    knob("r3d_span.small_dither_max_side", SMALL_MAX_SIDE_TUNE);
    knob("r3d_span.small_dither", 1);
    r3d_span_vertex_t v[3] = {{0, 0, 0.5F, 255, 0, 0},
                              {SMALL_MAX_SIDE_TUNE * R3D_SUBPIXEL, 0, 0.5F, 255, 0, 0},
                              {0, SMALL_MAX_SIDE_TUNE * R3D_SUBPIXEL, 0.5F, 255, 0, 0}};
    hot_triangle(&target, &v[0], &v[1], &v[2]);
    for (int i = 0; i < TEST_PIXELS; i++) {
        if (pixels[TEST_PIXELS + i]) {
            TEST_ASSERT_EQUAL_HEX16(GFX_RGB(0xFF0000), pixels[i]);
        }
    }
    v[1].r = 0;
    v[1].g = 255;
    v[2].r = 0;
    v[2].b = 255;
    int counts[3] = {0};
    int total = 0;
    float expected[3] = {0};
    for (int y = 0; y < 4 * R3D_SUBPIXEL; y++) {
        for (int x = 0; x < 4 * R3D_SUBPIXEL; x++) {
            (void)fixture();
            r3d_span_vertex_t moved[3];
            memcpy(moved, v, sizeof moved);
            for (int k = 0; k < 3; k++) {
                moved[k].x += x;
                moved[k].y += y;
            }
            if ((x + y) & 1) {
                const r3d_span_vertex_t swap = moved[1];
                moved[1] = moved[2];
                moved[2] = swap;
            }
            hot_triangle(&target, &moved[0], &moved[1], &moved[2]);
            for (int i = 0; i < TEST_PIXELS; i++) {
                if (!pixels[TEST_PIXELS + i]) {
                    continue;
                }
                const uint16_t choices[3] = {GFX_RGB(0xFF0000), GFX_RGB(0x00FF00), GFX_RGB(0x0000FF)};
                bool found = false;
                for (int k = 0; k < 3; k++) {
                    if (pixels[i] == choices[k]) {
                        counts[k]++;
                        found = true;
                    }
                }
                TEST_ASSERT_TRUE(found);
                const float b =
                    ((float)(i % TEST_SIDE) + 0.5F - (float)x / (float)R3D_SUBPIXEL) / (float)SMALL_MAX_SIDE_TUNE;
                const float c =
                    ((float)(i / TEST_SIDE) + 0.5F - (float)y / (float)R3D_SUBPIXEL) / (float)SMALL_MAX_SIDE_TUNE;
                expected[0] += 1.0F - b - c;
                expected[1] += b;
                expected[2] += c;
                total++;
            }
        }
    }
    for (int k = 0; k < 3; k++) {
        TEST_ASSERT_FLOAT_WITHIN(0.02F, expected[k] / (float)total, (float)counts[k] / (float)total);
    }
}

static void
test_small_dither_side_limit_coverage_and_solid_fill(void) {
    (void)fixture();
    knob("r3d_span.small_max_side", SMALL_MAX_SIDE_TUNE);
    knob("r3d_span.small_dither", SMALL_DITHER_TUNE);
    r3d_span_vertex_t v[3] = {{0, 0, 0.5F, 255, 0, 0},
                              {SMALL_MAX_SIDE_TUNE * R3D_SUBPIXEL, 0, 0.5F, 0, 255, 0},
                              {0, SMALL_MAX_SIDE_TUNE * R3D_SUBPIXEL, 0.5F, 0, 0, 255}};
    const uint16_t flat = GFX_RGB(0x555555);
    for (int limit = SMALL_MAX_SIDE_TUNE - 1; limit <= SMALL_MAX_SIDE_TUNE; limit++) {
        knob("r3d_span.small_dither_max_side", limit);
        for (int coverage = 0; coverage <= SMALL_DITHER_COVERAGE_EIGHTHS; coverage += 2) {
            knob("r3d_span.small_dither_coverage", coverage);
            const r3d_span_target_t target = fixture();
            hot_triangle(&target, &v[0], &v[1], &v[2]);
            for (int i = 0; i < TEST_PIXELS; i++) {
                if (!pixels[TEST_PIXELS + i]) {
                    continue;
                }
                const float b = ((float)(i % TEST_SIDE) + 0.5F) / (float)SMALL_MAX_SIDE_TUNE;
                const float c = ((float)(i / TEST_SIDE) + 0.5F) / (float)SMALL_MAX_SIDE_TUNE;
                const float edge_bias =
                    1.0F / (float)(SMALL_MAX_SIDE_TUNE * R3D_SUBPIXEL * SMALL_MAX_SIDE_TUNE * R3D_SUBPIXEL);
                const float a = 1.0F - b - c - edge_bias;
                const float low = a < b ? (a < c ? a : c) : (b < c ? b : c);
                const bool dither =
                    limit == SMALL_MAX_SIDE_TUNE
                    && TRIANGLE_VERTICES * SMALL_DITHER_COVERAGE_EIGHTHS * low < (float)coverage * (1.0F - edge_bias);
                TEST_ASSERT_EQUAL_INT(!dither, pixels[i] == flat);
            }
            memset(pixels, 0, sizeof(*pixels) * TEST_PIXELS * 2);
            hot_triangle_solid(&target, &v[0], &v[1], &v[2], flat);
            for (int i = 0; i < TEST_PIXELS; i++) {
                if (pixels[TEST_PIXELS + i]) {
                    TEST_ASSERT_EQUAL_HEX16(flat, pixels[i]);
                }
            }
        }
    }
}

static void
mark_writer(const r3d_span_writer_t* writer, const gfx_render_target_t* rows, int y, int first, int last, int32_t z,
            int32_t dz) {
    (void)writer;
    const uint16_t* depth = gfx_render_target_depth(rows, y);
    for (int x = first; x <= last; x++, z += dz) {
        if (depth[x] != R3D_DEPTH_EMPTY && depth[x] == (z >> R3D_DEPTH_SHIFT)) {
            pixels[TEST_PIXELS * 2 + y * TEST_SIDE + x]++;
        }
    }
}

static void
test_small_experiment_writer_matches_fill_depth(void) {
    (void)fixture();
    knob("r3d_span.small_flat_max_side", SMALL_MAX_SIDE_TUNE);
    knob("r3d_span.small_flat_spread", 1);
    const r3d_span_writer_t writer = {.span = mark_writer};
    for (int solid = 0; solid <= 1; solid++) {
        for (int spread = 0; spread <= 255; spread += 255) {
            r3d_span_vertex_t v[3] = {{0, 0, 0.2F, 0, 0, 0},
                                      {SMALL_MAX_SIDE_TUNE * R3D_SUBPIXEL, 0, 0.8F, (float)spread, 0, 0},
                                      {0, SMALL_MAX_SIDE_TUNE * R3D_SUBPIXEL, 0.5F, 0, 0, 0}};
            r3d_span_target_t target = fixture();
            target.writers = &writer;
            target.writer_count = 1;
            if (solid) {
                hot_triangle_solid(&target, &v[0], &v[1], &v[2], GFX_RGB(0x123456));
            } else {
                hot_triangle(&target, &v[0], &v[1], &v[2]);
            }
            hot_triangle_write(&target, &v[0], &v[1], &v[2], solid != 0);
            for (int i = 0; i < TEST_PIXELS; i++) {
                TEST_ASSERT_EQUAL_INT(pixels[TEST_PIXELS + i] != 0, pixels[TEST_PIXELS * 2 + i]);
            }
        }
    }
}

void
run_r3d_small_suite(void) {
    RUN_TEST(test_small_defaults_match_constant_fill);
    RUN_TEST(test_small_experiment_writer_matches_fill_depth);
    RUN_TEST(test_small_spread_channel_boundaries_and_faces);
    RUN_TEST(test_small_dither_equal_colours_and_vertex_shares);
    RUN_TEST(test_small_dither_side_limit_coverage_and_solid_fill);
}

SUITE_REGISTER(run_r3d_small_suite);
#endif
