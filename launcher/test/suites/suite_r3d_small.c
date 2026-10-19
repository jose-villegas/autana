#ifndef DEVICE_BUILD
#define TUNE_HOT_LIVE  1
#define r3d_span_begin hot_begin
#include "render/r3d_span.h"

/* Private selection and packing invariants need the live copy in this TU;
 * the separately compiled constant fill remains the default-image oracle. */
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
#include "asset/asset_pack.h"
#include "math/scalar/rng.h"
#include "suites.h"
#include "unity.h"

#define TEST_SIDE   24
#define TEST_PIXELS (TEST_SIDE * TEST_SIDE)

static void
knob(const char* name, int value) {
    TEST_ASSERT_TRUE(tune_set(tune_shared(), name, value));
    hot_begin();
}

static void
reset_knobs(void) {
    tune_registry_t* registry = tune_shared();
    for (const tune_entry_t* e = registry->first; e != NULL; e = e->next) {
        if (strncmp(e->name, "r3d_span.", strlen("r3d_span.")) == 0) {
            TEST_ASSERT_TRUE(tune_set(registry, e->name, e->initial));
        }
    }
    hot_begin();
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

#define ORACLE_SEED            937U
#define ORACLE_SAMPLES         32
#define ORACLE_RAMP_BITS       1
/* Colour+depth checksum recorded with merge-base a6740475c's r3d_span.c.
 * Re-record: bash launcher/tools/render/record_span_oracle.sh a6740475c
 * The constant-fill assertion prints the recorded checksum. */
#define DEFAULT_IMAGE_CHECKSUM 0x57208113U

static void
oracle_vertices(r3d_span_vertex_t v[TRIANGLE_VERTICES], rng_t* random, int side) {
    for (int k = 0; k < TRIANGLE_VERTICES; k++) {
        v[k] = (r3d_span_vertex_t){R3D_SUBPIXEL + rng_below(random, side * R3D_SUBPIXEL),
                                   R3D_SUBPIXEL + rng_below(random, side * R3D_SUBPIXEL),
                                   (float)(1 + rng_below(random, UINT8_MAX)) / (float)(UINT8_MAX + 1),
                                   (float)rng_below(random, UINT8_MAX + 1),
                                   (float)rng_below(random, UINT8_MAX + 1),
                                   (float)rng_below(random, UINT8_MAX + 1)};
    }
    v[0].x = R3D_SUBPIXEL;
    v[1].x = (side + 1) * R3D_SUBPIXEL;
}

static uint32_t
oracle_draw(const r3d_span_vertex_t v[TRIANGLE_VERTICES], rng_t* random, int winding, bool solid) {
    const r3d_span_target_t hot = fixture();
    for (int i = 0; i < TEST_PIXELS; i++) {
        pixels[i] = (uint16_t)rng_below(random, UINT16_MAX + 1);
        pixels[TEST_PIXELS + i] = (uint16_t)rng_below(random, R3D_DEPTH_NEAREST + 1);
    }
    memcpy(pixels + TEST_PIXELS * 2, pixels, sizeof(*pixels) * TEST_PIXELS * 2);
    const r3d_span_target_t normal =
        r3d_span_target(pixels + TEST_PIXELS * 2, pixels + TEST_PIXELS * 3, TEST_SIDE, 0, TEST_SIDE);
    const int b = winding ? 2 : 1;
    const int c = winding ? 1 : 2;
    if (solid) {
        hot_triangle_solid(&hot, &v[0], &v[b], &v[c], GFX_RGB(0x123456));
        r3d_span_triangle_solid(&normal, &v[0], &v[b], &v[c], GFX_RGB(0x123456));
    } else {
        hot_triangle(&hot, &v[0], &v[b], &v[c]);
        r3d_span_triangle(&normal, &v[0], &v[b], &v[c]);
    }
    TEST_ASSERT_EQUAL_MEMORY(pixels, pixels + TEST_PIXELS * 2, sizeof(*pixels) * TEST_PIXELS * 2);
    return asset_crc32(pixels + TEST_PIXELS * 2, sizeof(*pixels) * TEST_PIXELS * 2);
}

static void
test_small_defaults_match_constant_fill(void) {
    uint32_t checksum = ORACLE_SEED;
    for (int side = 1; side <= SMALL_MAX_SIDE_TUNE; side++) {
        for (int sample = 0; sample < ORACLE_SAMPLES; sample++) {
            rng_t random;
            rng_seed(&random, ORACLE_SEED + (uint32_t)(side * ORACLE_SAMPLES + sample));
            r3d_span_vertex_t v[TRIANGLE_VERTICES];
            oracle_vertices(v, &random, side);
            for (int winding = 0; winding < 2; winding++) {
                for (int solid = 0; solid < 2; solid++) {
                    checksum = rng_avalanche32(checksum ^ oracle_draw(v, &random, winding, solid != 0));
                }
            }
        }
    }
    TEST_ASSERT_EQUAL_HEX32(DEFAULT_IMAGE_CHECKSUM, checksum);
}

static void
test_small_packed_flat_rounds_channels(void) {
    const r3d_span_target_t target = fixture();
    knob("r3d_span.small_max_side", SMALL_MAX_SIDE_TUNE);
    knob("r3d_span.small_flat_spread_far", 1);
    knob("r3d_span.small_flat_spread_near", 1);
    const r3d_span_vertex_t v[TRIANGLE_VERTICES] = {{0, 0, 0.5F, 7, 3, 7},
                                                    {SMALL_MAX_SIDE_TUNE * R3D_SUBPIXEL, 0, 0.5F, 7, 3, 7},
                                                    {0, SMALL_MAX_SIDE_TUNE * R3D_SUBPIXEL, 0.5F, 15, 7, 15}};
    hot_triangle(&target, v, v + 1, v + 2);
    TEST_ASSERT_EQUAL_HEX16(gfx_color_rgb565(0, 0, 0), pixels[1]);
    for (unsigned a = 0; a <= GFX_RGB565_G6_MAX; a++) {
        for (unsigned b = 0; b <= GFX_RGB565_G6_MAX; b++) {
            for (unsigned c = 0; c <= GFX_RGB565_G6_MAX; c++) {
                const unsigned rb_max = gfx_rgb565_r5(UINT16_MAX);
                const uint16_t ca = gfx_color_rgb565((uint8_t)(a & rb_max), (uint8_t)a, (uint8_t)(c & rb_max));
                const uint16_t cb = gfx_color_rgb565((uint8_t)(b & rb_max), (uint8_t)b, (uint8_t)(a & rb_max));
                const uint16_t cc = gfx_color_rgb565((uint8_t)(c & rb_max), (uint8_t)c, (uint8_t)(b & rb_max));
                const unsigned rb = ((a & rb_max) + (b & rb_max) + (c & rb_max) + 1) / TRIANGLE_VERTICES;
                TEST_ASSERT_EQUAL_HEX16(
                    gfx_color_rgb565((uint8_t)rb, (uint8_t)((a + b + c + 1) / TRIANGLE_VERTICES), (uint8_t)rb),
                    gfx_color_rgb565_mean3(gfx_color_swap(ca), gfx_color_swap(cb), gfx_color_swap(cc)));
            }
        }
    }
}

static void
test_small_spread_channel_boundaries_and_faces(void) {
    (void)fixture();
    knob("r3d_span.small_max_side", SMALL_MAX_SIDE_TUNE);
    knob("r3d_span.small_flat_spread_far", 1);
    knob("r3d_span.small_flat_spread_near", 1);
    for (int channel = 0; channel < 3; channel++) {
        const float step = 2.0F * (float)(UINT8_MAX + 1)
                           / (float)(1 + (channel == 1 ? gfx_rgb565_g6(UINT16_MAX) : gfx_rgb565_r5(UINT16_MAX)));
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
            TEST_ASSERT_EQUAL_INT(delta < 0, flat);
            memset(pixels, 0, sizeof(*pixels) * TEST_PIXELS * 2);
            hot_triangle_solid(&target, &v[0], &v[1], &v[2], GFX_RGB(0x123456));
            TEST_ASSERT_EQUAL_UINT16(pixels[TEST_PIXELS + 1], pixels[TEST_PIXELS + 10]);
        }
    }
}

static void
test_small_gate_compares_quantized_channels(void) {
    (void)fixture();
    knob("r3d_span.small_max_side", SMALL_MAX_SIDE_TUNE);
    knob("r3d_span.small_flat_spread_near", 0);
    knob("r3d_span.small_flat_spread_far", 0);
    for (int channel = 0; channel < TRIANGLE_VERTICES; channel++) {
        const int channel_max = channel == 1 ? (int)GFX_RGB565_G6_MAX : (int)gfx_rgb565_r5(UINT16_MAX);
        const float step = (float)(UINT8_MAX + 1) / (float)(channel_max + 1);
        r3d_span_vertex_t v[TRIANGLE_VERTICES] = {{0, 0, 0.5F, 0, 0, 0},
                                                  {SMALL_MAX_SIDE_TUNE * R3D_SUBPIXEL, 0, 0.5F, 0, 0, 0},
                                                  {0, SMALL_MAX_SIDE_TUNE * R3D_SUBPIXEL, 0.5F, 0, 0, 0}};
        if (channel == 0) {
            v[1].r = step - 1;
        } else if (channel == 1) {
            v[1].g = step - 1;
        } else {
            v[1].b = step - 1;
        }
        const recti_t centres = {0, 0, SMALL_MAX_SIDE_TUNE, SMALL_MAX_SIDE_TUNE};
        TEST_ASSERT_EQUAL_INT(0, select_small(v, v + 1, v + 2, centres, false, NULL, NULL, NULL));
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

static bool
prepare_test_dither(small_dither_t* d, const r3d_span_vertex_t v[TRIANGLE_VERTICES]) {
    const uint16_t colors[TRIANGLE_VERTICES] = {r3d_span_pack_rgb(v[0].r, v[0].g, v[0].b),
                                                r3d_span_pack_rgb(v[1].r, v[1].g, v[1].b),
                                                r3d_span_pack_rgb(v[2].r, v[2].g, v[2].b)};
    return prepare_small_dither(d, v, v + 1, v + 2, true, SMALL_DITHER_COVERAGE_EIGHTHS, small_cell_bits, colors);
}

static void
test_small_two_by_two_dither_spans_all_ranks(void) {
    (void)fixture();
    knob("r3d_span.small_dither_cell_bits", 1);
    const r3d_span_vertex_t v[TRIANGLE_VERTICES] = {
        {0, 0, 0.5F, 255, 0, 0}, {R3D_SUBPIXEL, 0, 0.5F, 0, 255, 0}, {0, R3D_SUBPIXEL, 0.5F, 0, 0, 255}};
    small_dither_t d;
    TEST_ASSERT_TRUE(prepare_test_dither(&d, v));
    d.coverage_limit = d.total;
    int counts[TRIANGLE_VERTICES] = {0};
    const int32_t third = d.total / TRIANGLE_VERTICES;
    knob("r3d_span.small_dither_cell_bits", SMALL_DITHER_PATTERN_BITS_MAX);
    for (int y = 0; y <= d.phase_mask; y++) {
        for (int x = 0; x <= d.phase_mask; x++) {
            const uint16_t color =
                small_dither_pick(&d, gfx_dither4x4[y & d.phase_mask], NULL, x, third, third, d.total - 2 * third, 0);
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
    TEST_ASSERT_EQUAL_INT(0, select_small(&v[0], &v[1], &v[2], centres, false, NULL, NULL, NULL));
    centres.w++;
    TEST_ASSERT_EQUAL_INT(SMALL_FILL_WALK, select_small(&v[0], &v[1], &v[2], centres, false, NULL, NULL, NULL));
    knob("r3d_span.small_max_side", SMALL_MAX_SIDE_TUNE);
    TEST_ASSERT_EQUAL_INT(SMALL_FILL_WALK, select_small(&v[0], &v[1], &v[2], centres, false, NULL, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(0, select_small(&v[0], &v[1], &v[2], centres, true, NULL, NULL, NULL));
    knob("r3d_span.small_flat_spread_far", SMALL_FLAT_SPREAD_OFF);
    knob("r3d_span.small_flat_spread_near", SMALL_FLAT_SPREAD_OFF);
    TEST_ASSERT_EQUAL_INT(2, select_small(&v[0], &v[1], &v[2], centres, false, NULL, NULL, NULL));
    knob("r3d_span.small_dither", 0);
    TEST_ASSERT_EQUAL_INT(0, select_small(&v[0], &v[1], &v[2], centres, false, NULL, NULL, NULL));
    knob("r3d_span.small_dither", SMALL_DITHER_COVERAGE_EIGHTHS);
    knob("r3d_span.small_flat_spread_far", 1);
    knob("r3d_span.small_flat_spread_near", 1);
    for (int k = 0; k < TRIANGLE_VERTICES; k++) {
        v[k].r = (float)k;
        v[k].g = (float)k;
        v[k].b = (float)k;
    }
    centres.w = SMALL_MAX_SIDE;
    TEST_ASSERT_EQUAL_INT(0, select_small(&v[0], &v[1], &v[2], centres, false, NULL, NULL, NULL));
    knob("r3d_span.small_flat_spread_far", SMALL_FLAT_SPREAD_OFF);
    knob("r3d_span.small_flat_spread_near", SMALL_FLAT_SPREAD_OFF);
    small_dither_t d;
    TEST_ASSERT_FALSE(prepare_test_dither(&d, v));
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
        const float step = (float)(UINT8_MAX + 1) / (float)(gfx_rgb565_r5(UINT16_MAX) + 1);
        v[1].r = side <= SMALL_MAX_SIDE ? 0 : ((float)limit + 1) * step - 1;
        TEST_ASSERT_EQUAL_INT(coverage, select_small(&v[0], &v[1], &v[2], centres, false, NULL, NULL, NULL));
        if (side > SMALL_MAX_SIDE) {
            v[1].r += 1;
            TEST_ASSERT_EQUAL_INT(SMALL_FILL_WALK, select_small(&v[0], &v[1], &v[2], centres, false, NULL, NULL, NULL));
        }
    }
    knob("r3d_span.small_flat_spread_near", SMALL_FLAT_SPREAD_OFF);
    knob("r3d_span.small_flat_spread_far", SMALL_FLAT_SPREAD_OFF);
    v[1].r = UINT8_MAX;
    for (int side = SMALL_MAX_SIDE + 1; side <= SMALL_MAX_SIDE_TUNE; side++) {
        const recti_t centres = {0, 0, 1, side};
        TEST_ASSERT_EQUAL_INT(SMALL_DITHER_COVERAGE_EIGHTHS * (side - SMALL_MAX_SIDE)
                                  / (SMALL_MAX_SIDE_TUNE - SMALL_MAX_SIDE),
                              select_small(&v[0], &v[1], &v[2], centres, false, NULL, NULL, NULL));
    }
    knob("r3d_span.small_dither_ramp", 0);
    knob("r3d_span.small_flat_spread_far", 1);
    knob("r3d_span.small_flat_spread_near", 1);
    v[1].r = (float)UINT8_MAX / gfx_rgb565_r5(UINT16_MAX);
    const recti_t centres = {0, 0, SMALL_MAX_SIDE_TUNE, SMALL_MAX_SIDE_TUNE};
    TEST_ASSERT_EQUAL_INT(SMALL_DITHER_COVERAGE_EIGHTHS,
                          select_small(&v[0], &v[1], &v[2], centres, false, NULL, NULL, NULL));
    small_dither_t d;
    TEST_ASSERT_TRUE(prepare_test_dither(&d, v));
    knob("r3d_span.small_max_side", SMALL_MAX_SIDE + 1);
    knob("r3d_span.small_dither_ramp", 1);
    knob("r3d_span.small_flat_spread_near", 0);
    const recti_t narrow = {0, 0, SMALL_MAX_SIDE + 1, 1};
    TEST_ASSERT_EQUAL_INT(SMALL_DITHER_COVERAGE_EIGHTHS,
                          select_small(&v[0], &v[1], &v[2], narrow, false, NULL, NULL, NULL));
    knob("r3d_span.small_flat_spread_near", 1);
    knob("r3d_span.small_flat_spread_far", 0);
    TEST_ASSERT_EQUAL_INT(SMALL_FILL_WALK, select_small(&v[0], &v[1], &v[2], narrow, false, NULL, NULL, NULL));
}

#define EXPERIMENT_SIDE 8

static void
assert_clipped_guards(int first, int last) {
    for (int y = 0; y < TEST_SIDE; y++) {
        if (y >= first && y < last) {
            continue;
        }
        for (int x = 0; x < TEST_SIDE; x++) {
            const int i = y * TEST_SIDE + x;
            TEST_ASSERT_EQUAL_HEX16(0, pixels[i]);
            TEST_ASSERT_EQUAL_UINT16(i & 1 ? R3D_DEPTH_NEAREST : R3D_DEPTH_EMPTY, pixels[TEST_PIXELS + i]);
        }
    }
}

static void
assert_clipped_split(const r3d_span_vertex_t v[TRIANGLE_VERTICES], int winding) {
    (void)fixture();
    const int first = 2;
    const int last = TEST_SIDE - first;
    const int split = TEST_SIDE / 2;
    const int b = winding ? 2 : 1;
    const int c = winding ? 1 : 2;
    for (int i = 0; i < TEST_PIXELS; i++) {
        pixels[TEST_PIXELS + i] = i & 1 ? R3D_DEPTH_NEAREST : R3D_DEPTH_EMPTY;
    }
    memcpy(pixels + TEST_PIXELS * 2, pixels, sizeof(*pixels) * TEST_PIXELS * 2);
    const r3d_span_target_t full =
        r3d_span_target(pixels + first * TEST_SIDE, pixels + TEST_PIXELS + first * TEST_SIDE, TEST_SIDE, first, last);
    hot_triangle(&full, &v[0], &v[b], &v[c]);
    for (int part = 0; part < 2; part++) {
        const int row0 = part ? split : first;
        const int row1 = part ? last : split;
        const r3d_span_target_t window =
            r3d_span_target(pixels + TEST_PIXELS * 2 + row0 * TEST_SIDE, pixels + TEST_PIXELS * 3 + row0 * TEST_SIDE,
                            TEST_SIDE, row0, row1);
        hot_triangle(&window, &v[0], &v[b], &v[c]);
    }
    TEST_ASSERT_EQUAL_MEMORY(pixels, pixels + TEST_PIXELS * 2, sizeof(*pixels) * TEST_PIXELS * 2);
    assert_clipped_guards(first, last);
}

static void
assert_split_patterns(void) {
    for (int bits = 1; bits <= SMALL_DITHER_PATTERN_BITS_MAX; bits++) {
        knob("r3d_span.small_dither_cell_bits", bits);
        for (int offset = -EXPERIMENT_SIDE; offset < TEST_SIDE; offset++) {
            for (int winding = 0; winding < 2; winding++) {
                const r3d_span_vertex_t v[TRIANGLE_VERTICES] = {
                    {offset * R3D_SUBPIXEL, offset * R3D_SUBPIXEL, 0.2F, 255, 0, 0},
                    {(offset + EXPERIMENT_SIDE) * R3D_SUBPIXEL, offset * R3D_SUBPIXEL, 0.8F, 0, 255, 0},
                    {offset * R3D_SUBPIXEL, (offset + EXPERIMENT_SIDE) * R3D_SUBPIXEL, 0.5F, 0, 0, 255}};
                assert_clipped_split(v, winding);
            }
        }
    }
}

static void
test_small_experiment_clipped_split_rows(void) {
    (void)fixture();
    knob("r3d_span.small_max_side", EXPERIMENT_SIDE);
    for (int mode = 0; mode < 3; mode++) {
        knob("r3d_span.small_flat_spread_far", mode == 1 ? 1 : SMALL_FLAT_SPREAD_OFF);
        knob("r3d_span.small_flat_spread_near", mode == 1 ? 1 : SMALL_FLAT_SPREAD_OFF);
        knob("r3d_span.small_dither", mode == 2 ? SMALL_DITHER_COVERAGE_EIGHTHS : 0);
        assert_split_patterns();
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
assert_writer_matches_fill_depth(void) {
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

static void
test_small_experiment_writer_matches_fill_depth(void) {
    (void)fixture();
    knob("r3d_span.small_max_side", SMALL_MAX_SIDE_TUNE);
    knob("r3d_span.small_flat_spread_far", 1);
    knob("r3d_span.small_flat_spread_near", 1);
    for (int dither = 0; dither <= SMALL_DITHER_COVERAGE_EIGHTHS; dither += SMALL_DITHER_COVERAGE_EIGHTHS) {
        knob("r3d_span.small_dither", dither);
        knob("r3d_span.small_flat_spread_far", dither ? SMALL_FLAT_SPREAD_OFF : 1);
        knob("r3d_span.small_flat_spread_near", dither ? SMALL_FLAT_SPREAD_OFF : 1);
        assert_writer_matches_fill_depth();
    }
}

static void
test_small_grain_shades_distinct(void) {
    uint16_t shades[SMALL_GRAIN_AMPLITUDE_MAX][SMALL_GRAIN_LEVELS];
    const uint16_t middle = gfx_color_rgb565((uint8_t)(gfx_rgb565_r5(UINT16_MAX) / 2), (uint8_t)(GFX_RGB565_G6_MAX / 2),
                                             (uint8_t)(gfx_rgb565_b5(UINT16_MAX) / 2));
    for (int amplitude = 1; amplitude <= SMALL_GRAIN_AMPLITUDE_MAX; amplitude++) {
        prepare_small_grain(shades[amplitude - 1], middle, amplitude);
        for (int prior = 1; prior < amplitude; prior++) {
            TEST_ASSERT_TRUE(memcmp(shades[prior - 1], shades[amplitude - 1], sizeof shades[0]) != 0);
        }
    }
}

static void
test_small_grain_shades_neutral(void) {
    const int red = (int)gfx_rgb565_r5(UINT16_MAX) / 2;
    const int green = (int)GFX_RGB565_G6_MAX / 2;
    const int blue = (int)gfx_rgb565_b5(UINT16_MAX) / 2;
    const int rb_step = (UINT8_MAX + 1) / ((int)gfx_rgb565_r5(UINT16_MAX) + 1);
    const int g_step = (UINT8_MAX + 1) / ((int)GFX_RGB565_G6_MAX + 1);
    for (int amplitude = 1; amplitude <= SMALL_GRAIN_AMPLITUDE_MAX; amplitude++) {
        uint16_t shades[SMALL_GRAIN_LEVELS];
        prepare_small_grain(shades, gfx_color_rgb565((uint8_t)red, (uint8_t)green, (uint8_t)blue), amplitude);
        for (int level = 0; level < SMALL_GRAIN_LEVELS; level++) {
            const uint16_t native = gfx_color_swap(shades[level]);
            const int dr = ((int)gfx_rgb565_r5(native) - red) * rb_step;
            const int dg = ((int)gfx_rgb565_g6(native) - green) * g_step;
            const int db = ((int)gfx_rgb565_b5(native) - blue) * rb_step;
            TEST_ASSERT_TRUE(dr * dg > 0 && db * dg > 0);
            TEST_ASSERT_INT_WITHIN(rb_step, dg, dr);
            TEST_ASSERT_INT_WITHIN(rb_step, dg, db);
        }
    }
}

static uint16_t
reference_grain(uint16_t color, int amplitude, int x, int y) {
    const int bx = x & SMALL_GRAIN_PATTERN_MASK, by = y & SMALL_GRAIN_PATTERN_MASK;
    const int rank = 2 * (bx ^ by) + by;
    const float offset =
        (float)(amplitude * (2 * rank + 1 - SMALL_GRAIN_LEVELS)) / (float)(1 << SMALL_GRAIN_HALF_SHIFT);
    const int green_offset = offset < 0 ? -(int)(-offset + 0.5F) : (int)(offset + 0.5F);
    const float rb = (float)green_offset / (float)(1 << SMALL_GRAIN_RB_SHIFT);
    const int rb_offset = rb < 0 ? -(int)(-rb + 0.5F) : (int)(rb + 0.5F);
    const uint16_t native = gfx_color_swap(color);
    return gfx_color_rgb565(
        (uint8_t)mathi_clamp((int)gfx_rgb565_r5(native) + rb_offset, 0, (int)gfx_rgb565_r5(UINT16_MAX)),
        (uint8_t)mathi_clamp((int)gfx_rgb565_g6(native) + green_offset, 0, (int)GFX_RGB565_G6_MAX),
        (uint8_t)mathi_clamp((int)gfx_rgb565_b5(native) + rb_offset, 0, (int)gfx_rgb565_b5(UINT16_MAX)));
}

/* Scalar small-fill oracle, independent of production selection, threshold preparation and the pixel loop. */
static void
reference_small(const r3d_span_target_t* target, const r3d_span_vertex_t v[3], bool solid) {
    const r3d_span_extent_t extent = r3d_span_extent(v, v + 1, v + 2);
    const int side = mathi_max(extent.centres.w, extent.centres.h);
    bool selected = side <= small_max_side;
    int coverage = 0;
    int grain = 0;
    bool packed = false;
    if (selected && !solid && side > SMALL_MAX_SIDE) {
        const int extra = small_max_side - SMALL_MAX_SIDE;
        int spread = extra == 1 ? small_flat_spread_far : small_flat_spread_near;
        if (extra > 1) {
            spread += (small_flat_spread_far - small_flat_spread_near) * (side - SMALL_MAX_SIDE - 1) / (extra - 1);
        }
        float channels[3][3] = {{v[0].r, v[1].r, v[2].r}, {v[0].g, v[1].g, v[2].g}, {v[0].b, v[1].b, v[2].b}};
        for (int k = 0; k < TRIANGLE_VERTICES && spread != SMALL_FLAT_SPREAD_OFF; k++) {
            const int channel_max = k == 1 ? (int)GFX_RGB565_G6_MAX : (int)gfx_rgb565_r5(UINT16_MAX);
            const int scale = (UINT8_MAX + 1) / (channel_max + 1);
            for (int j = 0; j < TRIANGLE_VERTICES; j++) {
                channels[k][j] = (float)((int)channels[k][j] / scale);
            }
            float low = channels[k][0], high = low;
            for (int j = 1; j < TRIANGLE_VERTICES; j++) {
                low = channels[k][j] < low ? channels[k][j] : low;
                high = channels[k][j] > high ? channels[k][j] : high;
            }
            selected = selected && high - low <= (float)spread;
        }
        const tune_entry_t* grain_entry = tune_find(tune_shared(), "r3d_span.small_grain");
        if (grain_entry != NULL) {
            grain = *grain_entry->value;
            if (*tune_find(tune_shared(), "r3d_span.small_grain_ramp")->value) {
                grain = grain * (side - SMALL_MAX_SIDE) / mathi_max(extra, 1);
            }
        }
        coverage = small_dither_ramp ? small_dither * (side - SMALL_MAX_SIDE) / mathi_max(extra, 1) : small_dither;
        packed = spread != SMALL_FLAT_SPREAD_OFF || coverage != 0 || grain != 0;
    }
    if (!selected) {
        if (solid) {
            r3d_span_triangle_solid(target, v, v + 1, v + 2, GFX_RGB(0x123456));
        } else {
            r3d_span_triangle(target, v, v + 1, v + 2);
        }
        return;
    }
    const int32_t area = r3d_span_area2(v, v + 1, v + 2);
    if (!area) {
        return;
    }
    const int order[3] = {0, area > 0 ? 1 : 2, area > 0 ? 2 : 1};
    fill_t flat;
    set_flat(&flat, v, v + 1, v + 2);
    if (solid) {
        flat.flat_color = GFX_RGB(0x123456);
    }
    small_edge_t edges[3];
    uint16_t colors[3];
    int32_t total = area > 0 ? area : -area;
    for (int k = 0; k < TRIANGLE_VERTICES; k++) {
        edges[k] = small_edge(v + order[k], v + order[(k + 1) % TRIANGLE_VERTICES]);
        total -= edges[k].bias;
        const r3d_span_vertex_t* opposite = v + order[(k + 2) % TRIANGLE_VERTICES];
        colors[k] = r3d_span_pack_rgb(opposite->r, opposite->g, opposite->b);
    }
    if (packed && !grain) {
        unsigned sums[TRIANGLE_VERTICES] = {0};
        for (int k = 0; k < TRIANGLE_VERTICES; k++) {
            const uint16_t native = gfx_color_swap(colors[k]);
            sums[0] += gfx_rgb565_r5(native);
            sums[1] += gfx_rgb565_g6(native);
            sums[2] += gfx_rgb565_b5(native);
        }
        flat.flat_color =
            gfx_color_rgb565((uint8_t)((sums[0] + 1) / TRIANGLE_VERTICES), (uint8_t)((sums[1] + 1) / TRIANGLE_VERTICES),
                             (uint8_t)((sums[2] + 1) / TRIANGLE_VERTICES));
    }
    const int divisor = TRIANGLE_VERTICES * SMALL_DITHER_COVERAGE_EIGHTHS;
    const int32_t limit = (coverage * total + divisor - 1) / divisor;
    const recti_t box = r3d_span_window_box(target, extent.centres);
    for (int y = box.y; y < recti_bottom(box); y++) {
        for (int x = box.x; x < recti_right(box); x++) {
            int32_t edge[3];
            for (int k = 0; k < TRIANGLE_VERTICES; k++) {
                edge[k] = small_side(edges + k, x * R3D_SUBPIXEL + HALF_PIXEL, y * R3D_SUBPIXEL + HALF_PIXEL);
            }
            uint16_t* depth = gfx_render_target_depth(&target->rows, y) + x;
            if ((edge[0] | edge[1] | edge[2]) < 0 || flat.flat_z <= *depth) {
                continue;
            }
            uint16_t color = flat.flat_color;
            if (grain && !(colors[0] == colors[1] && colors[1] == colors[2])) {
                color = reference_grain(color, grain, x, y);
            }
            if (coverage && !(colors[0] == colors[1] && colors[1] == colors[2])
                && (edge[0] < limit || edge[1] < limit || edge[2] < limit)) {
                const int bits = small_dither_cell_bits, mask = (1 << bits) - 1;
                const int rank = gfx_dither4x4[y & mask][x & mask] >> (2 * (SMALL_DITHER_PATTERN_BITS_MAX - bits));
                const int32_t threshold = (total * (2 * rank + 1)) >> (1 + 2 * bits);
                color = colors[threshold < edge[0] ? 0 : threshold < edge[0] + edge[1] ? 1 : 2];
            }
            *depth = flat.flat_z;
            gfx_render_target_color(&target->rows, y)[x] = color;
        }
    }
}

static void
check_randomized_small(rng_t* random, r3d_span_vertex_t v[TRIANGLE_VERTICES], bool solid, bool reverse) {
    if (reverse) {
        const r3d_span_vertex_t swap = v[0];
        v[0] = v[2];
        v[2] = swap;
    }
    const r3d_span_target_t actual = r3d_span_target(pixels, pixels + TEST_PIXELS, TEST_SIDE, 0, TEST_SIDE);
    const r3d_span_target_t expected =
        r3d_span_target(pixels + 2 * TEST_PIXELS, pixels + 3 * TEST_PIXELS, TEST_SIDE, 0, TEST_SIDE);
    for (int i = 0; i < 2 * TEST_PIXELS; i++) {
        pixels[i] = (uint16_t)rng_below(random, UINT16_MAX + 1);
    }
    memcpy(pixels + 2 * TEST_PIXELS, pixels, sizeof(*pixels) * 2 * TEST_PIXELS);
    reference_small(&expected, v, solid);
    if (solid) {
        hot_triangle_solid(&actual, v, v + 1, v + 2, GFX_RGB(0x123456));
    } else {
        hot_triangle(&actual, v, v + 1, v + 2);
    }
    TEST_ASSERT_EQUAL_MEMORY(pixels + 2 * TEST_PIXELS, pixels, sizeof(*pixels) * 2 * TEST_PIXELS);
}

static void
test_small_randomized_reference(void) {
    (void)fixture();
    const int spreads[] = {0, 1, 4, SMALL_FLAT_SPREAD_OFF, SMALL_FLAT_SPREAD_OFF};
    for (int side = SMALL_MAX_SIDE; side <= SMALL_MAX_SIDE_TUNE; side++) {
        knob("r3d_span.small_max_side", side);
        for (int setting = 0; setting < (int)(sizeof spreads / sizeof spreads[0]); setting++) {
            knob("r3d_span.small_flat_spread_near", spreads[setting]);
            knob("r3d_span.small_flat_spread_far", spreads[setting == 4 ? 4 : 3 - setting]);
            for (int coverage = 0; coverage <= SMALL_DITHER_COVERAGE_EIGHTHS; coverage++) {
                knob("r3d_span.small_dither", coverage);
                for (int mode = 0; mode < 4; mode++) {
                    knob("r3d_span.small_dither_ramp", mode & 1);
                    knob("r3d_span.small_dither_cell_bits", 1 + (mode >> ORACLE_RAMP_BITS));
                    rng_t random;
                    rng_seed(&random, ORACLE_SEED + (uint32_t)(side * 100 + setting * 10 + mode));
                    for (int sample = 0; sample < ORACLE_SAMPLES; sample++) {
                        r3d_span_vertex_t v[3];
                        oracle_vertices(v, &random, 1 + (sample + setting + mode) % SMALL_MAX_SIDE_TUNE);
                        const int offset = (sample % 3 - 1) * side * R3D_SUBPIXEL;
                        for (int k = 0; k < TRIANGLE_VERTICES; k++) {
                            v[k].x += offset;
                            v[k].y += offset;
                            if (sample & 1) {
                                v[k].y = (v[k].y / R3D_SUBPIXEL) * R3D_SUBPIXEL + HALF_PIXEL;
                            }
                            if (sample % 4 == 0) {
                                v[k].r = v[k].g = v[k].b = 99.0F;
                            }
                        }
                        check_randomized_small(&random, v, sample % 5 == 0, (sample & 2) != 0);
                    }
                }
            }
        }
    }
}

static void
test_small_grain_randomized_reference(void) {
    (void)fixture();
    knob("r3d_span.small_max_side", SMALL_MAX_SIDE_TUNE);
    for (int amplitude = 0; amplitude <= SMALL_GRAIN_AMPLITUDE_MAX; amplitude++) {
        knob("r3d_span.small_grain", amplitude);
        for (int mode = 0; mode <= 1; mode++) {
            knob("r3d_span.small_grain_ramp", mode);
            for (int coverage = 0; coverage <= SMALL_DITHER_COVERAGE_EIGHTHS; coverage++) {
                knob("r3d_span.small_dither", coverage);
                knob("r3d_span.small_dither_cell_bits", 1 + (coverage & 1));
                knob("r3d_span.small_dither_ramp", coverage & 1);
                rng_t random;
                rng_seed(&random, ORACLE_SEED + (uint32_t)(amplitude * 100 + mode * 10 + coverage));
                for (int sample = 0; sample < ORACLE_SAMPLES * 2; sample++) {
                    r3d_span_vertex_t v[TRIANGLE_VERTICES];
                    oracle_vertices(v, &random, 1 + sample % SMALL_MAX_SIDE_TUNE);
                    for (int k = 0; k < TRIANGLE_VERTICES; k++) {
                        if (sample % 3 == 0) {
                            const float channel = sample & 1 ? (float)UINT8_MAX : 0.0F;
                            v[k].r = v[k].g = v[k].b = channel;
                            v[k].g += k == 0 ? (channel == 0.0F ? 12.0F : -12.0F) : 0.0F;
                        }
                        if (sample % 5 == 0) {
                            v[k].r = v[k].g = v[k].b = 99.0F;
                        }
                    }
                    check_randomized_small(&random, v, sample % 7 == 0, (sample & 1) != 0);
                }
            }
        }
    }
}

void
run_r3d_small_suite(void) {
    RUN_TEST(test_small_grain_shades_distinct);
    RUN_TEST(test_small_grain_shades_neutral);
    RUN_TEST(test_small_grain_randomized_reference);
    RUN_TEST(test_small_gate_compares_quantized_channels);
    RUN_TEST(test_small_randomized_reference);
    RUN_TEST(test_small_experiment_clipped_split_rows);
    RUN_TEST(test_small_gate_and_dither_ramps);
    RUN_TEST(test_small_defaults_match_constant_fill);
    RUN_TEST(test_small_packed_flat_rounds_channels);
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
