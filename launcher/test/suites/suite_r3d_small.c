#ifndef DEVICE_BUILD
#define r3d_span_begin hot_begin
#include "render/r3d_span.h"

/* A private copy exposes selection and kernel invariants without a public test API. */
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

#define TEST_SIDE              24
#define TEST_PIXELS            (TEST_SIDE * TEST_SIDE)

#define ORACLE_SEED            937U
#define ORACLE_SAMPLES         32
#define ORACLE_MAX_SIDE        16
#define LEGACY_FLAT_CHECKSUM   0x57208113U
#define LEGACY_DITHER_CHECKSUM 0xDA4716C8U
#define TEST_SOLID_COLOR       GFX_RGB(0x123456)
#define TEST_CHANNELS          3
#define TEST_RB_MAX            ((int)GFX_RGB565_G6_MAX >> 1)
#define TEST_GREEN_STEP        2
#define TEST_GREEN_SCALE       ((UINT8_MAX + 1) / (GFX_RGB565_G6_MAX + 1))

static uint16_t* pixels;

static void
cleanup(void) {
    hot_begin(SMALL_FILL_DEFAULT);
    free(pixels);
    pixels = NULL;
}

static r3d_span_target_t
fixture(void) {
    pixels = malloc(sizeof(*pixels) * TEST_PIXELS * 4);
    suite_set_test_cleanup(cleanup);
    TEST_ASSERT_NOT_NULL(pixels);
    memset(pixels, 0, sizeof(*pixels) * TEST_PIXELS * 4);
    return r3d_span_target(pixels, pixels + TEST_PIXELS, TEST_SIDE, 0, TEST_SIDE);
}

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

static void
draw_triangle(const r3d_span_target_t* target, const r3d_span_vertex_t v[TRIANGLE_VERTICES], bool solid) {
    if (solid) {
        hot_triangle_solid(target, v, v + 1, v + 2, TEST_SOLID_COLOR);
    } else {
        hot_triangle(target, v, v + 1, v + 2);
    }
}

/* Checksums pin colour and depth from the source revision recorded by record_span_oracle.sh. */
static uint32_t
legacy_checksum(bool dither) {
    const r3d_span_target_t target = fixture();
    hot_begin((small_fill_t){dither ? ORACLE_MAX_SIDE : R3D_SPAN_SMALL_MAX_SIDE, 0,
                             dither ? SMALL_FILL_DITHER : SMALL_FILL_FLAT});
    uint32_t checksum = ORACLE_SEED;
    for (int side = 1; side <= ORACLE_MAX_SIDE; side++) {
        for (int sample = 0; sample < ORACLE_SAMPLES; sample++) {
            rng_t random;
            rng_seed(&random, ORACLE_SEED + (uint32_t)(side * ORACLE_SAMPLES + sample));
            r3d_span_vertex_t v[TRIANGLE_VERTICES];
            oracle_vertices(v, &random, side);
            for (int winding = 0; winding < 2; winding++) {
                for (int solid = 0; solid < (dither ? 1 : 2); solid++) {
                    for (int i = 0; i < TEST_PIXELS; i++) {
                        pixels[i] = (uint16_t)rng_below(&random, UINT16_MAX + 1);
                        pixels[TEST_PIXELS + i] = (uint16_t)rng_below(&random, R3D_DEPTH_NEAREST + 1);
                    }
                    const r3d_span_vertex_t ordered[TRIANGLE_VERTICES] = {v[0], v[winding ? 2 : 1], v[winding ? 1 : 2]};
                    draw_triangle(&target, ordered, solid != 0);
                    checksum = rng_avalanche32(checksum ^ asset_crc32(pixels, sizeof(*pixels) * TEST_PIXELS * 2));
                }
            }
        }
    }
    return checksum;
}

static void
test_old_look_flat_is_bit_identical(void) {
    TEST_ASSERT_EQUAL_HEX32(LEGACY_FLAT_CHECKSUM, legacy_checksum(false));
}

static void
test_whole_triangle_dither_is_bit_identical(void) {
    TEST_ASSERT_EQUAL_HEX32(LEGACY_DITHER_CHECKSUM, legacy_checksum(true));
}

static int
reference_selector(const r3d_span_vertex_t v[TRIANGLE_VERTICES], int side, bool solid, small_fill_t fill) {
    if (side > fill.ceiling) {
        return SMALL_FILL_WALK;
    }
    if (solid || side <= R3D_SPAN_SMALL_MAX_SIDE) {
        return SMALL_FILL_FLAT;
    }
    if (fill.gate != 0) {
        const int intervals = fill.ceiling - R3D_SPAN_SMALL_MAX_SIDE - 1;
        const int offset = side - R3D_SPAN_SMALL_MAX_SIDE - 1;
        const int near = (int)GFX_RGB565_G6_MAX + 1;
        const int limit = intervals == 0 ? fill.gate : near - (near - (int)fill.gate) * offset / intervals;
        for (int channel = 0; channel < TEST_CHANNELS; channel++) {
            const int scale = TEST_GREEN_SCALE * (channel == 1 ? 1 : TEST_GREEN_STEP);
            int low = UINT8_MAX, high = 0;
            for (int corner = 0; corner < TRIANGLE_VERTICES; corner++) {
                const float value = channel == 0 ? v[corner].r : channel == 1 ? v[corner].g : v[corner].b;
                const int quantized = (int)value / scale;
                low = quantized < low ? quantized : low;
                high = quantized > high ? quantized : high;
            }
            if (high - low > limit) {
                return SMALL_FILL_WALK;
            }
        }
    }
    return fill.style;
}

static void
test_selector_random_triangles_and_gate_boundaries(void) {
    (void)fixture();
    static const uint8_t gates[] = {0, 1, 8, SMALL_FILL_GATE_DEFAULT, GFX_RGB565_G6_MAX, UINT8_MAX};
    rng_t random;
    rng_seed(&random, ORACLE_SEED);
    for (int ceiling = 0; ceiling <= ORACLE_MAX_SIDE; ceiling++) {
        for (size_t gate = 0; gate < sizeof gates / sizeof gates[0]; gate++) {
            for (small_fill_style_t style = SMALL_FILL_FLAT; style <= SMALL_FILL_DITHER; style++) {
                const small_fill_t fill = {(uint8_t)ceiling, gates[gate], style};
                hot_begin(fill);
                for (int side = 1; side <= ORACLE_MAX_SIDE + 1; side++) {
                    for (int sample = 0; sample < ORACLE_SAMPLES; sample++) {
                        r3d_span_vertex_t v[TRIANGLE_VERTICES];
                        oracle_vertices(v, &random, side);
                        for (int solid = 0; solid < 2; solid++) {
                            const r3d_span_extent_t e = r3d_span_extent(v, v + 1, v + 2);
                            const int box_side = mathi_max(e.centres.w, e.centres.h);
                            TEST_ASSERT_EQUAL_INT(reference_selector(v, box_side, solid != 0, fill),
                                                  select_small(v, v + 1, v + 2, e.centres, solid != 0, NULL));
                        }
                    }
                    /* Exercise both sides of every representable green-channel limit. */
                    for (unsigned spread = 0; spread <= GFX_RGB565_G6_MAX; spread++) {
                        r3d_span_vertex_t v[TRIANGLE_VERTICES] = {{0}};
                        v[1].g = (float)(spread * TEST_GREEN_SCALE);
                        const recti_t centres = {0, 0, side, side};
                        TEST_ASSERT_EQUAL_INT(reference_selector(v, side, false, fill),
                                              select_small(v, v + 1, v + 2, centres, false, NULL));
                    }
                }
            }
        }
    }
    hot_begin((small_fill_t){UINT8_MAX, SMALL_FILL_GATE_DEFAULT, SMALL_FILL_GRAIN});
    const r3d_span_vertex_t v[TRIANGLE_VERTICES] = {{0}};
    TEST_ASSERT_EQUAL_INT(SMALL_FILL_GRAIN,
                          select_small(v, v + 1, v + 2, (recti_t){0, 0, UINT8_MAX, UINT8_MAX}, false, NULL));
    TEST_ASSERT_EQUAL_INT(SMALL_FILL_WALK,
                          select_small(v, v + 1, v + 2, (recti_t){0, 0, UINT8_MAX + 1, UINT8_MAX}, false, NULL));
}

static uint16_t
reference_shade(uint16_t color) {
    const uint16_t native = gfx_color_swap(color);
    const int channels[] = {(int)gfx_rgb565_r5(native), (int)gfx_rgb565_g6(native), (int)gfx_rgb565_b5(native)};
    const int maxima[] = {TEST_RB_MAX, GFX_RGB565_G6_MAX, TEST_RB_MAX};
    const int steps[] = {1, TEST_GREEN_STEP, 1};
    const int brightness = TEST_GREEN_STEP * (channels[0] + channels[2]) + channels[1];
    const int full = TEST_GREEN_STEP * (maxima[0] + maxima[2]) + maxima[1];
    const int direction = 2 * brightness < full ? 1 : -1;
    for (int attempt = 0; attempt < 2; attempt++) {
        const int sign = attempt == 0 ? direction : -direction;
        int moved[TEST_CHANNELS];
        bool safe = true;
        for (int channel = 0; channel < TEST_CHANNELS; channel++) {
            moved[channel] = channels[channel] + sign * steps[channel];
            safe = safe && moved[channel] >= 0 && moved[channel] <= maxima[channel];
        }
        if (safe) {
            return gfx_color_rgb565((uint8_t)moved[0], (uint8_t)moved[1], (uint8_t)moved[2]);
        }
    }
    return color;
}

static void
test_grain_shade_preserves_channel_direction_without_overflow(void) {
    for (unsigned native = 0; native <= UINT16_MAX; native++) {
        const uint16_t color = gfx_color_swap((uint16_t)native);
        const uint16_t shade = small_grain_shade(color);
        TEST_ASSERT_EQUAL_HEX16(reference_shade(color), shade);
        const uint16_t moved = gfx_color_swap(shade);
        const int dr = (int)gfx_rgb565_r5(moved) - (int)gfx_rgb565_r5((uint16_t)native);
        const int dg = (int)gfx_rgb565_g6(moved) - (int)gfx_rgb565_g6((uint16_t)native);
        const int db = (int)gfx_rgb565_b5(moved) - (int)gfx_rgb565_b5((uint16_t)native);
        TEST_ASSERT_TRUE(dr >= -1 && dr <= 1);
        TEST_ASSERT_EQUAL_INT(dr, db);
        TEST_ASSERT_EQUAL_INT(TEST_GREEN_STEP * dr, dg);
    }
    TEST_ASSERT_EQUAL_HEX16(gfx_color_rgb565(1, 2, 1), small_grain_shade(GFX_RGB(0x000000)));
    TEST_ASSERT_EQUAL_HEX16(gfx_color_rgb565(TEST_RB_MAX - 1, GFX_RGB565_G6_MAX - TEST_GREEN_STEP, TEST_RB_MAX - 1),
                            small_grain_shade(GFX_RGB(0xffffff)));
    TEST_ASSERT_EQUAL_HEX16(GFX_RGB(0xff0000), small_grain_shade(GFX_RGB(0xff0000)));
}

static void
reference_small(const r3d_span_target_t* target, const r3d_span_vertex_t v[TRIANGLE_VERTICES], bool solid) {
    const r3d_span_extent_t extent = r3d_span_extent(v, v + 1, v + 2);
    const int style = reference_selector(v, mathi_max(extent.centres.w, extent.centres.h), solid, small_fill);
    if (style == SMALL_FILL_WALK) {
        const small_fill_t settings = small_fill;
        hot_begin((small_fill_t){0, 0, SMALL_FILL_FLAT});
        draw_triangle(target, v, solid);
        hot_begin(settings);
        return;
    }
    const int32_t area = r3d_span_area2(v, v + 1, v + 2);
    if (area == 0) {
        return;
    }
    const int order[TRIANGLE_VERTICES] = {0, area > 0 ? 1 : 2, area > 0 ? 2 : 1};
    const float third = 1.0F / (float)TRIANGLE_VERTICES;
    const uint16_t flat =
        solid ? TEST_SOLID_COLOR
              : r3d_span_pack_rgb((v[0].r + v[1].r + v[2].r) * third, (v[0].g + v[1].g + v[2].g) * third,
                                  (v[0].b + v[1].b + v[2].b) * third);
    const uint16_t z = (uint16_t)(mathf_clamp((v[0].z + v[1].z + v[2].z) * third, 0.0F, 1.0F) * R3D_DEPTH_NEAREST);
    small_edge_t edges[TRIANGLE_VERTICES];
    uint16_t colors[TRIANGLE_VERTICES];
    int32_t total = area > 0 ? area : -area;
    for (int k = 0; k < TRIANGLE_VERTICES; k++) {
        edges[k] = small_edge(v + order[k], v + order[(k + 1) % TRIANGLE_VERTICES]);
        total -= edges[k].bias;
        const r3d_span_vertex_t* opposite = v + order[(k + 2) % TRIANGLE_VERTICES];
        colors[k] = r3d_span_pack_rgb(opposite->r, opposite->g, opposite->b);
    }
    const recti_t box = r3d_span_window_box(target, extent.centres);
    for (int y = box.y; y < recti_bottom(box); y++) {
        for (int x = box.x; x < recti_right(box); x++) {
            int32_t edge[TRIANGLE_VERTICES];
            for (int k = 0; k < TRIANGLE_VERTICES; k++) {
                edge[k] = small_side(edges + k, x * R3D_SUBPIXEL + HALF_PIXEL, y * R3D_SUBPIXEL + HALF_PIXEL);
            }
            uint16_t* depth = gfx_render_target_depth(&target->rows, y) + x;
            if ((edge[0] | edge[1] | edge[2]) < 0 || z <= *depth) {
                continue;
            }
            uint16_t color = flat;
            if (style == SMALL_FILL_GRAIN && (x + y) % SMALL_GRAIN_PAIR != 0) {
                color = reference_shade(flat);
            }
            if (style == SMALL_FILL_DITHER && !(colors[0] == colors[1] && colors[1] == colors[2])) {
                const int pattern_side = (int)(sizeof gfx_dither4x4 / sizeof gfx_dither4x4[0]);
                const int rank = gfx_dither4x4[y % pattern_side][x % pattern_side];
                const int32_t threshold = total * (2 * rank + 1) / (2 * pattern_side * pattern_side);
                color = edge[0] == edge[1] && edge[1] == edge[2] ? flat
                                                                 : colors[threshold < edge[0]             ? 0
                                                                          : threshold < edge[0] + edge[1] ? 1
                                                                                                          : 2];
            }
            *depth = z;
            gfx_render_target_color(&target->rows, y)[x] = color;
        }
    }
}

static void
test_style_kernels_randomized_reference(void) {
    const r3d_span_target_t actual = fixture();
    const r3d_span_target_t expected =
        r3d_span_target(pixels + 2 * TEST_PIXELS, pixels + 3 * TEST_PIXELS, TEST_SIDE, 0, TEST_SIDE);
    static const uint8_t gates[] = {0, SMALL_FILL_GATE_DEFAULT, 1};
    for (small_fill_style_t style = SMALL_FILL_FLAT; style <= SMALL_FILL_DITHER; style++) {
        for (size_t gate = 0; gate < sizeof gates / sizeof gates[0]; gate++) {
            hot_begin((small_fill_t){ORACLE_MAX_SIDE, gates[gate], style});
            for (int side = 1; side <= ORACLE_MAX_SIDE + 1; side++) {
                for (int sample = 0; sample < ORACLE_SAMPLES; sample++) {
                    rng_t random;
                    rng_seed(&random, ORACLE_SEED + (uint32_t)(side * ORACLE_SAMPLES + sample));
                    r3d_span_vertex_t v[TRIANGLE_VERTICES];
                    oracle_vertices(v, &random, side);
                    for (int corner = 0; corner < TRIANGLE_VERTICES; corner++) {
                        v[corner].x += (sample % SMALL_GRAIN_PAIR) * R3D_SUBPIXEL;
                    }
                    if (sample & 1) {
                        const r3d_span_vertex_t swap = v[1];
                        v[1] = v[2];
                        v[2] = swap;
                    }
                    for (int solid = 0; solid < 2; solid++) {
                        for (int i = 0; i < TEST_PIXELS * 2; i++) {
                            pixels[i] = (uint16_t)rng_below(&random, UINT16_MAX + 1);
                        }
                        memcpy(pixels + TEST_PIXELS * 2, pixels, sizeof(*pixels) * TEST_PIXELS * 2);
                        reference_small(&expected, v, solid != 0);
                        draw_triangle(&actual, v, solid != 0);
                        TEST_ASSERT_EQUAL_MEMORY(pixels + TEST_PIXELS * 2, pixels, sizeof(*pixels) * TEST_PIXELS * 2);
                    }
                }
            }
        }
    }
}

void
run_r3d_small_suite(void) {
    RUN_TEST(test_old_look_flat_is_bit_identical);
    RUN_TEST(test_whole_triangle_dither_is_bit_identical);
    RUN_TEST(test_selector_random_triangles_and_gate_boundaries);
    RUN_TEST(test_grain_shade_preserves_channel_direction_without_overflow);
    RUN_TEST(test_style_kernels_randomized_reference);
}

SUITE_REGISTER(run_r3d_small_suite);
#endif
