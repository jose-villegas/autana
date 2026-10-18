#ifndef DEVICE_BUILD
#include "render/r3d_span.h"

/* Private selection and packing invariants need the live copy in this TU;
 * the separately compiled constant fill remains the default-image oracle. */
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
#include "math/scalar/rng.h"
#include "suites.h"
#include "unity.h"

#define TEST_SIDE   24
#define TEST_PIXELS (TEST_SIDE * TEST_SIDE)

static void
knob(const char* name, int value) {
    TEST_ASSERT_TRUE(tune_set(tune_shared(), name, value));
}

static void
ignore_reply(const char* line) {
    (void)line;
}

static void
reset_knobs(void) {
    for (const tune_entry_t* e = tune_shared()->first; e != NULL; e = e->next) {
        if (strncmp(e->name, "r3d_span.", strlen("r3d_span.")) == 0) {
            char line[TUNE_NAME_MAX + sizeof "RESET "];
            snprintf(line, sizeof line, "RESET %s", e->name);
            TEST_ASSERT_TRUE(tune_handle_line(line, ignore_reply));
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
    rng_t random;
    for (int i = 0; i < 1000; i++) {
        rng_seed(&random, 937 + (uint32_t)i);
        r3d_span_vertex_t v[3];
        for (int k = 0; k < 3; k++) {
            v[k] = (r3d_span_vertex_t){rng_below(&random, TEST_SIDE * R3D_SUBPIXEL),
                                       rng_below(&random, TEST_SIDE * R3D_SUBPIXEL),
                                       0.2F + (float)rng_below(&random, UINT8_MAX + 1) / 512.0F,
                                       (float)rng_below(&random, UINT8_MAX + 1),
                                       (float)rng_below(&random, UINT8_MAX + 1),
                                       (float)rng_below(&random, UINT8_MAX + 1)};
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
    knob("r3d_span.small_max_side", SMALL_MAX_SIDE_TUNE);
    knob("r3d_span.small_flat_spread_far", 1);
    knob("r3d_span.small_flat_spread_near", 1);
    for (int channel = 0; channel < 3; channel++) {
        const float step =
            (float)UINT8_MAX / (channel == 1 ? (float)gfx_rgb565_g6(UINT16_MAX) : (float)gfx_rgb565_r5(UINT16_MAX));
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
assert_uniform_colour(uint16_t colour) {
    for (int i = 0; i < TEST_PIXELS; i++) {
        if (pixels[TEST_PIXELS + i]) {
            TEST_ASSERT_EQUAL_HEX16(colour, pixels[i]);
        }
    }
}

static void
move_triangle(r3d_span_vertex_t moved[TRIANGLE_VERTICES], const r3d_span_vertex_t v[TRIANGLE_VERTICES], int dx,
              int dy) {
    for (int k = 0; k < TRIANGLE_VERTICES; k++) {
        moved[k] = v[k];
        moved[k].x += dx;
        moved[k].y += dy;
    }
}

static int
accumulate_shares(int x, int y, int counts[TRIANGLE_VERTICES], float expected[TRIANGLE_VERTICES]) {
    int total = 0;
    for (int i = 0; i < TEST_PIXELS; i++) {
        if (!pixels[TEST_PIXELS + i]) {
            continue;
        }
        const uint16_t choices[TRIANGLE_VERTICES] = {GFX_RGB(0xFF0000), GFX_RGB(0x00FF00), GFX_RGB(0x0000FF)};
        bool found = false;
        for (int k = 0; k < TRIANGLE_VERTICES; k++) {
            if (pixels[i] == choices[k]) {
                counts[k]++;
                found = true;
            }
        }
        TEST_ASSERT_TRUE(found);
        const float b = ((float)(i % TEST_SIDE) + 0.5F - (float)x / (float)R3D_SUBPIXEL) / (float)SMALL_MAX_SIDE_TUNE;
        const float c = ((float)(i / TEST_SIDE) + 0.5F - (float)y / (float)R3D_SUBPIXEL) / (float)SMALL_MAX_SIDE_TUNE;
        expected[0] += 1.0F - b - c;
        expected[1] += b;
        expected[2] += c;
        total++;
    }
    return total;
}

static void
test_small_dither_uniform_colours(void) {
    const r3d_span_target_t target = fixture();
    knob("r3d_span.small_max_side", SMALL_MAX_SIDE_TUNE);
    knob("r3d_span.small_dither", SMALL_DITHER_COVERAGE_EIGHTHS);
    const r3d_span_vertex_t v[TRIANGLE_VERTICES] = {{0, 0, 0.5F, 255, 0, 0},
                                                    {SMALL_MAX_SIDE_TUNE * R3D_SUBPIXEL, 0, 0.5F, 255, 0, 0},
                                                    {0, SMALL_MAX_SIDE_TUNE * R3D_SUBPIXEL, 0.5F, 255, 0, 0}};
    hot_triangle(&target, &v[0], &v[1], &v[2]);
    assert_uniform_colour(GFX_RGB(0xFF0000));
}

static void
test_small_dither_vertex_shares(void) {
    const r3d_span_target_t target = fixture();
    knob("r3d_span.small_flat_spread_far", gfx_rgb565_g6(UINT16_MAX));
    knob("r3d_span.small_flat_spread_near", gfx_rgb565_g6(UINT16_MAX));
    knob("r3d_span.small_max_side", SMALL_MAX_SIDE_TUNE);
    knob("r3d_span.small_dither", SMALL_DITHER_COVERAGE_EIGHTHS);
    const r3d_span_vertex_t v[TRIANGLE_VERTICES] = {{0, 0, 0.5F, 255, 0, 0},
                                                    {SMALL_MAX_SIDE_TUNE * R3D_SUBPIXEL, 0, 0.5F, 0, 255, 0},
                                                    {0, SMALL_MAX_SIDE_TUNE * R3D_SUBPIXEL, 0.5F, 0, 0, 255}};
    int counts[TRIANGLE_VERTICES] = {0};
    int total = 0;
    float expected[TRIANGLE_VERTICES] = {0};
    for (int y = 0; y < 4 * R3D_SUBPIXEL; y++) {
        for (int x = 0; x < 4 * R3D_SUBPIXEL; x++) {
            (void)fixture();
            r3d_span_vertex_t moved[TRIANGLE_VERTICES];
            move_triangle(moved, v, x, y);
            if ((x + y) & 1) {
                const r3d_span_vertex_t swap = moved[1];
                moved[1] = moved[2];
                moved[2] = swap;
            }
            hot_triangle(&target, &moved[0], &moved[1], &moved[2]);
            total += accumulate_shares(x, y, counts, expected);
        }
    }
    for (int k = 0; k < TRIANGLE_VERTICES; k++) {
        TEST_ASSERT_FLOAT_WITHIN(0.02F, expected[k] / (float)total, (float)counts[k] / (float)total);
    }
}

static void
assert_dither_coverage(int coverage, uint16_t flat) {
    for (int i = 0; i < TEST_PIXELS; i++) {
        if (!pixels[TEST_PIXELS + i]) {
            continue;
        }
        const float b = ((float)(i % TEST_SIDE) + 0.5F) / (float)SMALL_MAX_SIDE_TUNE;
        const float c = ((float)(i / TEST_SIDE) + 0.5F) / (float)SMALL_MAX_SIDE_TUNE;
        const float edge_bias = 1.0F / (float)(SMALL_MAX_SIDE_TUNE * R3D_SUBPIXEL * SMALL_MAX_SIDE_TUNE * R3D_SUBPIXEL);
        const float a = 1.0F - b - c - edge_bias;
        const float low = a < b ? (a < c ? a : c) : (b < c ? b : c);
        const bool dither =
            TRIANGLE_VERTICES * SMALL_DITHER_COVERAGE_EIGHTHS * low < (float)coverage * (1.0F - edge_bias);
        TEST_ASSERT_EQUAL_INT(!dither, pixels[i] == flat);
    }
}

static void
test_small_dither_coverage_and_solid_fill(void) {
    (void)fixture();
    knob("r3d_span.small_max_side", SMALL_MAX_SIDE_TUNE);
    knob("r3d_span.small_dither", SMALL_DITHER_COVERAGE_EIGHTHS);
    r3d_span_vertex_t v[3] = {{0, 0, 0.5F, 255, 0, 0},
                              {SMALL_MAX_SIDE_TUNE * R3D_SUBPIXEL, 0, 0.5F, 0, 255, 0},
                              {0, SMALL_MAX_SIDE_TUNE * R3D_SUBPIXEL, 0.5F, 0, 0, 255}};
    const uint16_t flat = GFX_RGB(0x555555);
    for (int coverage = 0; coverage <= SMALL_DITHER_COVERAGE_EIGHTHS; coverage += 2) {
        knob("r3d_span.small_dither", coverage);
        const r3d_span_target_t target = fixture();
        hot_triangle(&target, &v[0], &v[1], &v[2]);
        assert_dither_coverage(coverage, flat);
        memset(pixels, 0, sizeof(*pixels) * TEST_PIXELS * 2);
        hot_triangle_solid(&target, &v[0], &v[1], &v[2], flat);
        assert_uniform_colour(flat);
    }
}

static void
assert_translation(int dx, int dy) {
    for (int y = 0; y < SMALL_MAX_SIDE_TUNE; y++) {
        for (int x = 0; x < SMALL_MAX_SIDE_TUNE; x++) {
            const int original = y * TEST_SIDE + x;
            const int translated = original + dx + dy * TEST_SIDE;
            TEST_ASSERT_EQUAL_HEX16(pixels[TEST_PIXELS * 2 + original], pixels[translated]);
            TEST_ASSERT_EQUAL_UINT16(pixels[TEST_PIXELS * 3 + original], pixels[TEST_PIXELS + translated]);
        }
    }
}

static void
test_small_dither_pattern_repeats_on_both_axes(void) {
    const r3d_span_target_t target = fixture();
    knob("r3d_span.small_max_side", SMALL_MAX_SIDE_TUNE);
    knob("r3d_span.small_dither", SMALL_DITHER_COVERAGE_EIGHTHS);
    for (int bits = 1; bits <= 2; bits++) {
        knob("r3d_span.small_dither_cell_bits", bits);
        memset(pixels, 0, sizeof(*pixels) * TEST_PIXELS * 2);
        const r3d_span_vertex_t v[3] = {{0, 0, 0.5F, 255, 0, 0},
                                        {SMALL_MAX_SIDE_TUNE * R3D_SUBPIXEL, 0, 0.5F, 0, 255, 0},
                                        {0, SMALL_MAX_SIDE_TUNE * R3D_SUBPIXEL, 0.5F, 0, 0, 255}};
        hot_triangle(&target, &v[0], &v[1], &v[2]);
        memcpy(pixels + TEST_PIXELS * 2, pixels, sizeof(*pixels) * TEST_PIXELS * 2);
        for (int axis = 0; axis < 2; axis++) {
            r3d_span_vertex_t moved[TRIANGLE_VERTICES];
            const int dx = axis == 0 ? (1 << bits) : 0;
            const int dy = axis == 1 ? (1 << bits) : 0;
            move_triangle(moved, v, dx * R3D_SUBPIXEL, dy * R3D_SUBPIXEL);
            memset(pixels, 0, sizeof(*pixels) * TEST_PIXELS * 2);
            hot_triangle(&target, &moved[0], &moved[1], &moved[2]);
            assert_translation(dx, dy);
        }
    }
}

static void
test_small_two_by_two_dither_spans_all_ranks(void) {
    (void)fixture();
    knob("r3d_span.small_dither_cell_bits", 1);
    const r3d_span_vertex_t v[TRIANGLE_VERTICES] = {
        {0, 0, 0.5F, 255, 0, 0}, {R3D_SUBPIXEL, 0, 0.5F, 0, 255, 0}, {0, R3D_SUBPIXEL, 0.5F, 0, 0, 255}};
    small_dither_t d;
    TEST_ASSERT_TRUE(prepare_small_dither(&d, &v[0], &v[1], &v[2], true, SMALL_DITHER_COVERAGE_EIGHTHS));
    d.coverage_limit = d.total;
    int counts[TRIANGLE_VERTICES] = {0};
    const int32_t third = d.total / TRIANGLE_VERTICES;
    for (int y = 0; y < (1 << small_dither_cell_bits); y++) {
        for (int x = 0; x < (1 << small_dither_cell_bits); x++) {
            const uint16_t color = small_dither_pick(&d, x, y, third, third, d.total - 2 * third, 0);
            for (int i = 0; i < TRIANGLE_VERTICES; i++) {
                counts[i] += color == d.color[i];
            }
        }
    }
    TEST_ASSERT_EQUAL_INT(1, counts[0]);
    TEST_ASSERT_EQUAL_INT(2, counts[1]);
    TEST_ASSERT_EQUAL_INT(1, counts[2]);
}

static void
test_small_edge_sum_is_constant(void) {
    (void)fixture();
    rng_t random;
    rng_seed(&random, 891);
    for (int sample = 0; sample < 100; sample++) {
        r3d_span_vertex_t v[3] = {0};
        for (int k = 0; k < 3; k++) {
            v[k].x = rng_below(&random, SMALL_MAX_SIDE_TUNE * R3D_SUBPIXEL);
            v[k].y = rng_below(&random, SMALL_MAX_SIDE_TUNE * R3D_SUBPIXEL);
        }
        const small_edge_t e[3] = {small_edge(&v[0], &v[1]), small_edge(&v[1], &v[2]), small_edge(&v[2], &v[0])};
        const int32_t total = r3d_span_area2(&v[0], &v[1], &v[2]) - e[0].bias - e[1].bias - e[2].bias;
        for (int y = 0; y < TEST_SIDE; y++) {
            for (int x = 0; x < TEST_SIDE; x++) {
                const int32_t cx = x * R3D_SUBPIXEL + HALF_PIXEL;
                const int32_t cy = y * R3D_SUBPIXEL + HALF_PIXEL;
                TEST_ASSERT_EQUAL_INT32(total, small_side(&e[0], cx, cy) + small_side(&e[1], cx, cy)
                                                   + small_side(&e[2], cx, cy));
            }
        }
    }
}

static void
test_small_selection_and_uniform_packing(void) {
    (void)fixture();
    r3d_span_vertex_t v[3] = {{0, 0, 0.5F, 255, 0, 0},
                              {SMALL_MAX_SIDE * R3D_SUBPIXEL, 0, 0.5F, 0, 255, 0},
                              {0, SMALL_MAX_SIDE * R3D_SUBPIXEL, 0.5F, 0, 0, 255}};
    recti_t centres = {0, 0, SMALL_MAX_SIDE, SMALL_MAX_SIDE};
    knob("r3d_span.small_flat_spread_far", 0);
    knob("r3d_span.small_flat_spread_near", 0);
    knob("r3d_span.small_dither", 2);
    TEST_ASSERT_EQUAL_INT(0, select_small(&v[0], &v[1], &v[2], centres, false));
    centres.w++;
    TEST_ASSERT_EQUAL_INT(SMALL_FILL_WALK, select_small(&v[0], &v[1], &v[2], centres, false));
    knob("r3d_span.small_max_side", SMALL_MAX_SIDE_TUNE);
    TEST_ASSERT_EQUAL_INT(SMALL_FILL_WALK, select_small(&v[0], &v[1], &v[2], centres, false));
    TEST_ASSERT_EQUAL_INT(0, select_small(&v[0], &v[1], &v[2], centres, true));
    knob("r3d_span.small_flat_spread_far", SMALL_FLAT_SPREAD_OFF);
    knob("r3d_span.small_flat_spread_near", SMALL_FLAT_SPREAD_OFF);
    TEST_ASSERT_EQUAL_INT(2, select_small(&v[0], &v[1], &v[2], centres, false));
    knob("r3d_span.small_dither", 0);
    TEST_ASSERT_EQUAL_INT(0, select_small(&v[0], &v[1], &v[2], centres, false));
    knob("r3d_span.small_dither", SMALL_DITHER_COVERAGE_EIGHTHS);
    knob("r3d_span.small_flat_spread_far", 1);
    knob("r3d_span.small_flat_spread_near", 1);
    for (int k = 0; k < TRIANGLE_VERTICES; k++) {
        v[k].r = (float)k;
        v[k].g = (float)k;
        v[k].b = (float)k;
    }
    centres.w = SMALL_MAX_SIDE;
    TEST_ASSERT_EQUAL_INT(0, select_small(&v[0], &v[1], &v[2], centres, false));
    knob("r3d_span.small_flat_spread_far", SMALL_FLAT_SPREAD_OFF);
    knob("r3d_span.small_flat_spread_near", SMALL_FLAT_SPREAD_OFF);
    small_dither_t d;
    TEST_ASSERT_FALSE(prepare_small_dither(&d, &v[0], &v[1], &v[2], true, SMALL_DITHER_COVERAGE_EIGHTHS));
}

static void
test_small_gate_and_dither_ramps(void) {
    (void)fixture();
    knob("r3d_span.small_max_side", SMALL_MAX_SIDE_TUNE);
    knob("r3d_span.small_flat_spread_near", 0);
    knob("r3d_span.small_flat_spread_far", SMALL_MAX_SIDE_TUNE - SMALL_MAX_SIDE - 1);
    knob("r3d_span.small_dither", SMALL_DITHER_COVERAGE_EIGHTHS);
    knob("r3d_span.small_dither_ramp", 1);
    r3d_span_vertex_t v[TRIANGLE_VERTICES] = {
        {0, 0, 0.5F, 0, 0, 0}, {R3D_SUBPIXEL, 0, 0.5F, 0, 0, 0}, {0, R3D_SUBPIXEL, 0.5F, 0, 0, 0}};
    for (int side = 1; side <= SMALL_MAX_SIDE_TUNE; side++) {
        const recti_t centres = {0, 0, side, 1};
        const int coverage = side <= SMALL_MAX_SIDE ? 0
                                                    : SMALL_DITHER_COVERAGE_EIGHTHS * (side - SMALL_MAX_SIDE)
                                                          / (SMALL_MAX_SIDE_TUNE - SMALL_MAX_SIDE);
        const int limit = side - SMALL_MAX_SIDE - 1;
        v[1].r = side <= SMALL_MAX_SIDE ? 0 : ((float)limit * UINT8_MAX) / gfx_rgb565_r5(UINT16_MAX);
        TEST_ASSERT_EQUAL_INT(coverage, select_small(&v[0], &v[1], &v[2], centres, false));
        if (side > SMALL_MAX_SIDE) {
            v[1].r += 1;
            TEST_ASSERT_EQUAL_INT(SMALL_FILL_WALK, select_small(&v[0], &v[1], &v[2], centres, false));
        }
    }
    knob("r3d_span.small_flat_spread_near", SMALL_FLAT_SPREAD_OFF);
    knob("r3d_span.small_flat_spread_far", SMALL_FLAT_SPREAD_OFF);
    v[1].r = UINT8_MAX;
    for (int side = SMALL_MAX_SIDE + 1; side <= SMALL_MAX_SIDE_TUNE; side++) {
        const recti_t centres = {0, 0, 1, side};
        TEST_ASSERT_EQUAL_INT(SMALL_DITHER_COVERAGE_EIGHTHS * (side - SMALL_MAX_SIDE)
                                  / (SMALL_MAX_SIDE_TUNE - SMALL_MAX_SIDE),
                              select_small(&v[0], &v[1], &v[2], centres, false));
    }
    knob("r3d_span.small_dither_ramp", 0);
    knob("r3d_span.small_flat_spread_far", 1);
    knob("r3d_span.small_flat_spread_near", 1);
    v[1].r = (float)UINT8_MAX / gfx_rgb565_r5(UINT16_MAX);
    const recti_t centres = {0, 0, SMALL_MAX_SIDE_TUNE, SMALL_MAX_SIDE_TUNE};
    TEST_ASSERT_EQUAL_INT(SMALL_DITHER_COVERAGE_EIGHTHS, select_small(&v[0], &v[1], &v[2], centres, false));
    small_dither_t d;
    TEST_ASSERT_TRUE(prepare_small_dither(&d, &v[0], &v[1], &v[2], true, SMALL_DITHER_COVERAGE_EIGHTHS));
    knob("r3d_span.small_max_side", SMALL_MAX_SIDE + 1);
    knob("r3d_span.small_dither_ramp", 1);
    knob("r3d_span.small_flat_spread_near", 0);
    const recti_t narrow = {0, 0, SMALL_MAX_SIDE + 1, 1};
    TEST_ASSERT_EQUAL_INT(SMALL_DITHER_COVERAGE_EIGHTHS, select_small(&v[0], &v[1], &v[2], narrow, false));
    knob("r3d_span.small_flat_spread_near", 1);
    knob("r3d_span.small_flat_spread_far", 0);
    TEST_ASSERT_EQUAL_INT(SMALL_FILL_WALK, select_small(&v[0], &v[1], &v[2], narrow, false));
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
    knob("r3d_span.small_max_side", SMALL_MAX_SIDE_TUNE);
    knob("r3d_span.small_flat_spread_far", 1);
    knob("r3d_span.small_flat_spread_near", 1);
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
    RUN_TEST(test_small_gate_and_dither_ramps);
    RUN_TEST(test_small_defaults_match_constant_fill);
    RUN_TEST(test_small_dither_pattern_repeats_on_both_axes);
    RUN_TEST(test_small_two_by_two_dither_spans_all_ranks);
    RUN_TEST(test_small_edge_sum_is_constant);
    RUN_TEST(test_small_selection_and_uniform_packing);
    RUN_TEST(test_small_experiment_writer_matches_fill_depth);
    RUN_TEST(test_small_spread_channel_boundaries_and_faces);
    RUN_TEST(test_small_dither_uniform_colours);
    RUN_TEST(test_small_dither_vertex_shares);
    RUN_TEST(test_small_dither_coverage_and_solid_fill);
}

SUITE_REGISTER(run_r3d_small_suite);
#endif
