#ifndef DEVICE_BUILD
#define r3d_span_small_fill_resolve hot_resolve
#define r3d_span_small_fill_default hot_default
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
#include "hash/crc32.h"
#include "image/pixel/bayer.h"
#include "image/pixel/rgb565.h"
#include "math/scalar/rng.h"
#include "render/raster_attachment.h"
#include "suites.h"
#include "unity.h"

#define TEST_SIDE                  24
#define TEST_PIXELS                (TEST_SIDE * TEST_SIDE)

#define ORACLE_SEED                937U
#define ORACLE_SAMPLES             32
#define ORACLE_MAX_SIDE            SMALL_FILL_CEILING_MAX
#define CEILING_FOUR_FLAT_CHECKSUM 0x57208113U
#define VERTEX_DITHER_CHECKSUM     0xDA4716C8U
#define TEST_SOLID_COLOR           RGB565BE(0x123456)
#define TEST_CHANNELS              3
#define TEST_RB_MAX                ((int)RGB565_G6_MAX >> 1)
#define TEST_GREEN_STEP            2
#define TEST_GREEN_SCALE           ((UINT8_MAX + 1) / (RGB565_G6_MAX + 1))

static uint16_t* pixels;
static r3d_span_small_fill_t* resolved;
static small_fill_t settings;

static void
resolve_fill(small_fill_t fill) {
    settings = fill;
    r3d_span_small_fill_resolve(resolved, fill);
}

static void
cleanup(void) {
    free(resolved);
    resolved = NULL;
    free(pixels);
    pixels = NULL;
}

static r3d_span_target_t
fixture(void) {
    pixels = malloc(sizeof(*pixels) * TEST_PIXELS * 4);
    suite_set_test_cleanup(cleanup);
    TEST_ASSERT_NOT_NULL(pixels);
    memset(pixels, 0, sizeof(*pixels) * TEST_PIXELS * 4);
    resolved = malloc(sizeof(*resolved));
    TEST_ASSERT_NOT_NULL(resolved);
    resolve_fill(SMALL_FILL_SMOOTH);
    r3d_span_target_t target = r3d_span_target(pixels, pixels + TEST_PIXELS, TEST_SIDE, 0, TEST_SIDE);
    target.small_fill = resolved;
    return target;
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
span_checksum(bool dither) {
    const r3d_span_target_t target = fixture();
    if (dither) {
        resolve_fill((small_fill_t){ORACLE_MAX_SIDE, 0, SMALL_FILL_DITHER});
    }
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
                    checksum = rng_avalanche32(checksum ^ crc32_update(0, pixels, sizeof(*pixels) * TEST_PIXELS * 2));
                }
            }
        }
    }
    return checksum;
}

static void
test_ceiling_four_flat_is_bit_identical(void) {
    TEST_ASSERT_EQUAL_INT(SMALL_FILL_SMOOTH.ceiling, hot_default.ceiling);
    TEST_ASSERT_EQUAL_INT(SMALL_FILL_SMOOTH.style, hot_default.style);
    TEST_ASSERT_EQUAL_HEX32(CEILING_FOUR_FLAT_CHECKSUM, span_checksum(false));
}

static void
test_whole_triangle_dither_is_bit_identical(void) {
    TEST_ASSERT_EQUAL_HEX32(VERTEX_DITHER_CHECKSUM, span_checksum(true));
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
        const int near = (int)RGB565_G6_MAX + 1;
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
    static const uint8_t gates[] = {0, 1, 8, SMALL_FILL_GATE_FAST, RGB565_G6_MAX, UINT8_MAX};
    rng_t random;
    rng_seed(&random, ORACLE_SEED);
    for (int ceiling = 0; ceiling <= ORACLE_MAX_SIDE; ceiling++) {
        for (size_t gate = 0; gate < sizeof gates / sizeof gates[0]; gate++) {
            for (small_fill_style_t style = SMALL_FILL_FLAT; style < SMALL_FILL_STYLE_COUNT; style++) {
                const small_fill_t fill = {(uint8_t)ceiling, gates[gate], style};
                resolve_fill(fill);
                for (int side = 1; side <= ORACLE_MAX_SIDE + 1; side++) {
                    for (int sample = 0; sample < ORACLE_SAMPLES; sample++) {
                        r3d_span_vertex_t v[TRIANGLE_VERTICES];
                        oracle_vertices(v, &random, side);
                        for (int solid = 0; solid < 2; solid++) {
                            const r3d_span_extent_t e = r3d_span_extent(v, v + 1, v + 2);
                            const int box_side = mathi_max(e.centres.w, e.centres.h);
                            TEST_ASSERT_EQUAL_INT(reference_selector(v, box_side, solid != 0, fill),
                                                  select_small(v, v + 1, v + 2, e.centres, solid != 0, resolved));
                        }
                    }
                    /* Exercise both sides of every representable green-channel limit. */
                    for (unsigned spread = 0; spread <= RGB565_G6_MAX; spread++) {
                        r3d_span_vertex_t v[TRIANGLE_VERTICES] = {{0}};
                        v[1].g = (float)(spread * TEST_GREEN_SCALE);
                        const recti_t centres = {0, 0, side, side};
                        TEST_ASSERT_EQUAL_INT(reference_selector(v, side, false, fill),
                                              select_small(v, v + 1, v + 2, centres, false, resolved));
                    }
                }
            }
        }
    }
    resolve_fill((small_fill_t){UINT8_MAX, SMALL_FILL_GATE_FAST, SMALL_FILL_GRAIN});
    TEST_ASSERT_EQUAL_INT(SMALL_FILL_CEILING_MAX, resolved->ceiling);
    const r3d_span_vertex_t v[TRIANGLE_VERTICES] = {{0}};
    TEST_ASSERT_EQUAL_INT(SMALL_FILL_GRAIN,
                          select_small(v, v + 1, v + 2, (recti_t){0, 0, SMALL_FILL_CEILING_MAX, SMALL_FILL_CEILING_MAX},
                                       false, resolved));
    TEST_ASSERT_EQUAL_INT(SMALL_FILL_WALK,
                          select_small(v, v + 1, v + 2, (recti_t){0, 0, UINT8_MAX + 1, UINT8_MAX}, false, resolved));
}

static uint16_t
reference_shade(uint16_t color) {
    const uint16_t native = rgb565_swap(color);
    const int channels[] = {(int)rgb565_r5(native), (int)rgb565_g6(native), (int)rgb565_b5(native)};
    const int maxima[] = {TEST_RB_MAX, RGB565_G6_MAX, TEST_RB_MAX};
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
            return rgb565be_pack((uint8_t)moved[0], (uint8_t)moved[1], (uint8_t)moved[2]);
        }
    }
    return color;
}

static void
test_grain_shade_preserves_channel_direction_without_overflow(void) {
    for (unsigned native = 0; native <= UINT16_MAX; native++) {
        const uint16_t color = rgb565_swap((uint16_t)native);
        const uint16_t shade = small_grain_shade(color);
        TEST_ASSERT_EQUAL_HEX16(reference_shade(color), shade);
        const uint16_t moved = rgb565_swap(shade);
        const int dr = (int)rgb565_r5(moved) - (int)rgb565_r5((uint16_t)native);
        const int dg = (int)rgb565_g6(moved) - (int)rgb565_g6((uint16_t)native);
        const int db = (int)rgb565_b5(moved) - (int)rgb565_b5((uint16_t)native);
        TEST_ASSERT_TRUE(dr >= -1 && dr <= 1);
        TEST_ASSERT_EQUAL_INT(dr, db);
        TEST_ASSERT_EQUAL_INT(TEST_GREEN_STEP * dr, dg);
    }
    TEST_ASSERT_EQUAL_HEX16(rgb565be_pack(1, 2, 1), small_grain_shade(RGB565BE(0x000000)));
    TEST_ASSERT_EQUAL_HEX16(rgb565be_pack(TEST_RB_MAX - 1, RGB565_G6_MAX - TEST_GREEN_STEP, TEST_RB_MAX - 1),
                            small_grain_shade(RGB565BE(0xffffff)));
    TEST_ASSERT_EQUAL_HEX16(RGB565BE(0xff0000), small_grain_shade(RGB565BE(0xff0000)));
}

static __attribute__((noinline)) void
reference_small(const r3d_span_target_t* target, const r3d_span_vertex_t v[TRIANGLE_VERTICES], bool solid) {
    const r3d_span_extent_t extent = r3d_span_extent(v, v + 1, v + 2);
    const int style = reference_selector(v, mathi_max(extent.centres.w, extent.centres.h), solid, settings);
    if (style == SMALL_FILL_WALK) {
        const r3d_span_small_fill_t walked = {0};
        r3d_span_target_t walk_target = *target;
        walk_target.small_fill = &walked;
        draw_triangle(&walk_target, v, solid);
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
            uint16_t* depth = gfx_target_depth(&target->rows, y) + x;
            if ((edge[0] | edge[1] | edge[2]) < 0 || z <= *depth) {
                continue;
            }
            uint16_t color = flat;
            if (style == SMALL_FILL_GRAIN && (x + y) % SMALL_GRAIN_PAIR != 0) {
                color = reference_shade(flat);
            }
            if (style == SMALL_FILL_DITHER && !(colors[0] == colors[1] && colors[1] == colors[2])) {
                const int pattern_side = (int)(sizeof bayer4x4 / sizeof bayer4x4[0]);
                const int rank = bayer4x4[y % pattern_side][x % pattern_side];
                const int32_t threshold = total * (2 * rank + 1) / (2 * pattern_side * pattern_side);
                color = edge[0] == edge[1] && edge[1] == edge[2] ? flat
                                                                 : colors[threshold < edge[0]             ? 0
                                                                          : threshold < edge[0] + edge[1] ? 1
                                                                                                          : 2];
            }
            *depth = z;
            gfx_target_color(&target->rows, y)[x] = color;
        }
    }
}

static void
test_style_kernels_randomized_reference(void) {
    const r3d_span_target_t actual = fixture();
    r3d_span_target_t expected =
        r3d_span_target(pixels + 2 * TEST_PIXELS, pixels + 3 * TEST_PIXELS, TEST_SIDE, 0, TEST_SIDE);
    expected.small_fill = resolved;
    static const uint8_t gates[] = {0, SMALL_FILL_GATE_FAST, 1};
    for (small_fill_style_t style = SMALL_FILL_FLAT; style < SMALL_FILL_STYLE_COUNT; style++) {
        for (size_t gate = 0; gate < sizeof gates / sizeof gates[0]; gate++) {
            resolve_fill((small_fill_t){ORACLE_MAX_SIDE, gates[gate], style});
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

static void
test_grain_screen_phase_with_odd_window_and_known_shades(void) {
    r3d_span_target_t actual = fixture();
    actual.rows.first_row = 1;
    for (int i = 0; i < actual.rows.count; i++) {
        actual.rows.attachment[i].height--;
    }
    actual.rows.attachment[GFX_ATTACHMENT_COLOR].pixels = pixels + TEST_SIDE;
    actual.rows.attachment[GFX_ATTACHMENT_DEPTH].pixels = pixels + TEST_PIXELS + TEST_SIDE;
    resolve_fill((small_fill_t){ORACLE_MAX_SIDE, 0, SMALL_FILL_GRAIN});
    const r3d_span_vertex_t v[TRIANGLE_VERTICES] = {{-R3D_SUBPIXEL, 0, 0.5F, 0, 0, 0},
                                                    {(ORACLE_MAX_SIDE - 1) * R3D_SUBPIXEL, 0, 0.5F, 0, 0, 0},
                                                    {-R3D_SUBPIXEL, ORACLE_MAX_SIDE * R3D_SUBPIXEL, 0.5F, 0, 0, 0}};
    draw_triangle(&actual, v, false);
    TEST_ASSERT_EQUAL_HEX16(rgb565be_pack(1, TEST_GREEN_STEP, 1), pixels[TEST_SIDE]);
    TEST_ASSERT_EQUAL_HEX16(RGB565BE(0x000000), pixels[TEST_SIDE + 1]);
    r3d_span_target_t expected = actual;
    expected.rows.attachment[GFX_ATTACHMENT_COLOR].pixels = pixels + TEST_PIXELS * 2 + TEST_SIDE;
    expected.rows.attachment[GFX_ATTACHMENT_DEPTH].pixels = pixels + TEST_PIXELS * 3 + TEST_SIDE;
    reference_small(&expected, v, false);
    TEST_ASSERT_EQUAL_MEMORY(pixels + TEST_PIXELS * 2, pixels, TEST_PIXELS * 2 * sizeof(*pixels));
}

static void
test_dither_equal_packed_colours_and_equal_edges_use_flat(void) {
    r3d_span_target_t actual = fixture();
    r3d_span_vertex_t v[TRIANGLE_VERTICES] = {{0, 0, 0.5F, 80, 80, 80},
                                              {ORACLE_MAX_SIDE * R3D_SUBPIXEL, 0, 0.5F, 81, 81, 81},
                                              {0, ORACLE_MAX_SIDE * R3D_SUBPIXEL, 0.5F, 82, 82, 82}};
    resolve_fill((small_fill_t){ORACLE_MAX_SIDE, 0, SMALL_FILL_DITHER});
    draw_triangle(&actual, v, false);
    r3d_span_target_t expected =
        r3d_span_target(pixels + TEST_PIXELS * 2, pixels + TEST_PIXELS * 3, TEST_SIDE, 0, TEST_SIDE);
    expected.small_fill = resolved;
    resolve_fill((small_fill_t){ORACLE_MAX_SIDE, 0, SMALL_FILL_FLAT});
    draw_triangle(&expected, v, false);
    TEST_ASSERT_EQUAL_MEMORY(pixels + TEST_PIXELS * 2, pixels, TEST_PIXELS * 2 * sizeof(*pixels));
    memset(pixels, 0, TEST_PIXELS * 4 * sizeof(*pixels));
    const int centre = (ORACLE_MAX_SIDE / 2) * R3D_SUBPIXEL + HALF_PIXEL;
    const int reach = (R3D_SPAN_SMALL_MAX_SIDE + 1) * R3D_SUBPIXEL;
    v[0] = (r3d_span_vertex_t){centre + reach, centre, 0.5F, UINT8_MAX, 0, 0};
    v[1] = (r3d_span_vertex_t){centre, centre + 1, 0.5F, 0, UINT8_MAX, 0};
    v[2] = (r3d_span_vertex_t){centre - reach + 1, centre - 1, 0.5F, 0, 0, UINT8_MAX};
    for (int edge = 0; edge < TRIANGLE_VERTICES; edge++) {
        const small_edge_t line = small_edge(v + edge, v + (edge + 1) % TRIANGLE_VERTICES);
        TEST_ASSERT_EQUAL_INT(reach - 1, small_side(&line, centre, centre));
    }
    resolve_fill((small_fill_t){ORACLE_MAX_SIDE, 0, SMALL_FILL_DITHER});
    draw_triangle(&actual, v, false);
    resolve_fill((small_fill_t){ORACLE_MAX_SIDE, 0, SMALL_FILL_FLAT});
    draw_triangle(&expected, v, false);
    const int pixel = (ORACLE_MAX_SIDE / 2) * (TEST_SIDE + 1);
    TEST_ASSERT_NOT_EQUAL(R3D_DEPTH_EMPTY, pixels[TEST_PIXELS + pixel]);
    TEST_ASSERT_EQUAL_HEX16(pixels[TEST_PIXELS * 2 + pixel], pixels[pixel]);
}

static void
test_writer_and_draw_select_the_same_depth_under_every_style(void) {
    r3d_span_target_t target = fixture();
    const r3d_span_writer_t writer = {
        .span = raster_attachment_tag, .attachment = GFX_ATTACHMENT_FURTHER, .value = ORACLE_SEED};
    target.writers = &writer;
    target.writer_count = 1;
    target.rows.attachment[GFX_ATTACHMENT_FURTHER] = target.rows.attachment[GFX_ATTACHMENT_COLOR];
    target.rows.attachment[GFX_ATTACHMENT_FURTHER].pixels = pixels + TEST_PIXELS * 2;
    target.rows.attachment[GFX_ATTACHMENT_FURTHER].format = IMAGE_U16;
    target.rows.count = GFX_ATTACHMENT_FURTHER + 1;
    static const uint8_t gates[] = {0, SMALL_FILL_GATE_FAST, UINT8_MAX};
    for (small_fill_style_t style = SMALL_FILL_FLAT; style < SMALL_FILL_STYLE_COUNT; style++) {
        for (size_t gate = 0; gate < sizeof gates / sizeof gates[0]; gate++) {
            resolve_fill((small_fill_t){ORACLE_MAX_SIDE, gates[gate], style});
            for (int spread = SMALL_FILL_GATE_FAST - 1; spread <= SMALL_FILL_GATE_FAST + 1; spread++) {
                memset(pixels, 0, TEST_PIXELS * 4 * sizeof(*pixels));
                const r3d_span_vertex_t v[TRIANGLE_VERTICES] = {
                    {0, 0, 0.25F, 80, 0, 80},
                    {ORACLE_MAX_SIDE * R3D_SUBPIXEL, 0, 0.5F, 80, spread * TEST_GREEN_SCALE, 80},
                    {0, ORACLE_MAX_SIDE * R3D_SUBPIXEL, 0.75F, 80, 0, 80}};
                draw_triangle(&target, v, false);
                hot_triangle_write(&target, v, v + 1, v + 2, false);
                int covered = 0;
                for (int i = 0; i < TEST_PIXELS; i++) {
                    const bool drawn = pixels[TEST_PIXELS + i] != R3D_DEPTH_EMPTY;
                    TEST_ASSERT_EQUAL_UINT16(drawn ? ORACLE_SEED : 0, pixels[TEST_PIXELS * 2 + i]);
                    covered += drawn;
                }
                TEST_ASSERT_GREATER_THAN(0, covered);
            }
        }
    }
}

void
run_r3d_small_suite(void) {
    RUN_TEST(test_grain_screen_phase_with_odd_window_and_known_shades);
    RUN_TEST(test_dither_equal_packed_colours_and_equal_edges_use_flat);
    RUN_TEST(test_writer_and_draw_select_the_same_depth_under_every_style);
    RUN_TEST(test_ceiling_four_flat_is_bit_identical);
    RUN_TEST(test_whole_triangle_dither_is_bit_identical);
    RUN_TEST(test_selector_random_triangles_and_gate_boundaries);
    RUN_TEST(test_grain_shade_preserves_channel_direction_without_overflow);
    RUN_TEST(test_style_kernels_randomized_reference);
}

SUITE_REGISTER(run_r3d_small_suite);
#endif
