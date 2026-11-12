#ifndef DEVICE_BUILD
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "asset/asset_pack.h"
#include "core/tune.h"
#include "image/pixel/bayer.h"
#include "image/pixel/rgb565.h"
#include "math/scalar/mathi.h"
#include "math/scalar/rng.h"
#include "render/r3d_span.h"
#include "render/r3d_span_internal.h"
#include "render/texture/r3d_texture.h"
#include "suites.h"
#include "unity.h"

#define SIDE         48
#define PIXELS       (SIDE * SIDE)
#define TEX_LOG2     3
#define TEX_SIDE     (1 << TEX_LOG2)
#define TEX_LEVELS   3
#define PALETTE_OFF  R3D_TEXTURE_HEADER_SIZE
#define TEXELS_OFF   (PALETTE_OFF + (R3D_TEXTURE_COLOURS * R3D_TEXTURE_PALETTE_STRIDE))
#define TEX_BYTES    (TEXELS_OFF + 32 + 8 + 2)
#define RANDOM_SEED  4111U
#define RANDOM_TRIS  300
#define WHITE_INDEX  15
#define MEAN_INDEX   8 /* i * 16 == R3D_TEXTURE_ONE */
#define TINT_GREY    128
#define NEAR_DEPTH   0.5F
#define DEPTH_SPREAD 64

static uint16_t* buffers; /* colour then depth, twice */
static uint8_t* ltex;
static r3d_texture_t* texture;

static void
cleanup(void) {
    free(buffers);
    free(ltex);
    free(texture);
    buffers = NULL;
    ltex = NULL;
    texture = NULL;
}

static void
put_u16(uint8_t* at, uint16_t v) {
    memcpy(at, &v, sizeof v);
}

static void
put_u32(uint8_t* at, uint32_t v) {
    memcpy(at, &v, sizeof v);
}

/* An 8x8 INDEX4 texture with two mips: texel (x, y) of level 0 holds
 * index (x + y) & 15, each mip the index of its level; colour i is i * 16
 * in every channel, 15 being 1.0 white. */
static void
write_ltex(uint8_t* b) {
    memset(b, 0, TEX_BYTES);
    put_u16(b + R3D_TEXTURE_AT_VERSION, R3D_TEXTURE_VERSION);
    put_u16(b + R3D_TEXTURE_AT_FORMAT, R3D_TEXTURE_INDEX4);
    b[R3D_TEXTURE_AT_SIZE_LOG2] = TEX_LOG2;
    b[R3D_TEXTURE_AT_LEVELS] = TEX_LEVELS;
    b[R3D_TEXTURE_AT_TINT] = TINT_GREY;
    b[R3D_TEXTURE_AT_TINT + 1] = TINT_GREY;
    b[R3D_TEXTURE_AT_TINT + 2] = TINT_GREY;
    put_u32(b + R3D_TEXTURE_AT_PALETTE_OFF, PALETTE_OFF);
    put_u32(b + R3D_TEXTURE_AT_TEXELS_OFF, TEXELS_OFF);
    for (int i = 0; i < R3D_TEXTURE_COLOURS; i++) {
        const uint8_t level = i == WHITE_INDEX ? R3D_TEXTURE_ONE : (uint8_t)(i * 16);
        memset(b + PALETTE_OFF + (i * R3D_TEXTURE_PALETTE_STRIDE), level, 3);
    }
    uint8_t* at = b + TEXELS_OFF;
    for (int l = 0; l < TEX_LEVELS; l++) {
        const int side = TEX_SIDE >> l;
        for (int y = 0; y < side; y++) {
            for (int x = 0; x < side; x++) {
                const int o = (y * side) + x;
                const int index = l == 0 ? (x + y) & 15 : l;
                at[o >> 1] |= (uint8_t)(index << ((o & 1) * 4));
            }
        }
        at += (side * side) / 2;
    }
}

static void
fixture(void) {
    buffers = calloc(PIXELS * 4, sizeof *buffers);
    ltex = malloc(TEX_BYTES);
    texture = malloc(sizeof *texture);
    suite_set_test_cleanup(cleanup);
    TEST_ASSERT_NOT_NULL(buffers);
    TEST_ASSERT_NOT_NULL(ltex);
    TEST_ASSERT_NOT_NULL(texture);
    write_ltex(ltex);
    const asset_view_t view = {ltex, TEX_BYTES};
    TEST_ASSERT_EQUAL(ASSET_OK, r3d_texture_from_asset(&view, texture));
}

static r3d_span_target_t
target_at(int which) {
    uint16_t* color = buffers + (which * 2 * PIXELS);
    return r3d_span_target(color, color + PIXELS, SIDE, 0, SIDE);
}

static void
clear(void) {
    memset(buffers, 0, sizeof(*buffers) * PIXELS * 4);
}

static r3d_span_vertex_t
vertex(float x, float y, float z, float grey) {
    return (r3d_span_vertex_t){r3d_span_snap(x), r3d_span_snap(y), z, grey, grey, grey};
}

static void
random_triangle(rng_t* random, r3d_span_vertex_t v[3], r3d_span_uv_t uv[3]) {
    for (int k = 0; k < 3; k++) {
        v[k] = (r3d_span_vertex_t){(int32_t)rng_below(random, (SIDE + 16) * R3D_SUBPIXEL) - (8 * R3D_SUBPIXEL),
                                   (int32_t)rng_below(random, (SIDE + 16) * R3D_SUBPIXEL) - (8 * R3D_SUBPIXEL),
                                   NEAR_DEPTH + ((float)rng_below(random, DEPTH_SPREAD) / 4096.0F),
                                   (float)rng_below(random, 256),
                                   (float)rng_below(random, 256),
                                   (float)rng_below(random, 256)};
        uv[k] = (r3d_span_uv_t){(float)rng_below(random, 64) - 32.0F, (float)rng_below(random, 64) - 32.0F};
    }
}

/* Every pixel a plain fill draws a textured fill draws, and no other: the
 * same edges decide coverage. Depth stays in a narrow band, as a sliver's
 * steeper depth step is clamped by the plain fill (its STEP_MAX) and can
 * leave it writing depth 0. */
static void
test_textured_covers_what_plain_covers(void) {
    fixture();
    rng_t random;
    rng_seed(&random, RANDOM_SEED);
    for (int t = 0; t < RANDOM_TRIS; t++) {
        r3d_span_vertex_t v[3];
        r3d_span_uv_t uv[3];
        random_triangle(&random, v, uv);
        clear();
        const r3d_span_target_t plain = target_at(0);
        const r3d_span_target_t textured = target_at(1);
        r3d_span_triangle(&plain, &v[0], &v[1], &v[2]);
        r3d_span_textured_triangle(&textured, &v[0], &v[1], &v[2], uv, texture);
        for (int i = 0; i < PIXELS; i++) {
            TEST_ASSERT_EQUAL_MESSAGE(buffers[PIXELS + i] != R3D_DEPTH_EMPTY,
                                      buffers[(3 * PIXELS) + i] != R3D_DEPTH_EMPTY, "coverage differs");
        }
    }
}

/* Two triangles sharing an edge, each filled alone, cover their square with
 * no pixel in both and none missed: the square a plain fill of both covers. */
static void
test_shared_edge_neither_cracks_nor_overlaps(void) {
    fixture();
    const r3d_span_uv_t uv[3] = {{0, 0}, {8, 0}, {8, 8}};
    const r3d_span_vertex_t a = vertex(3.3F, 2.7F, 0.5F, 100), b = vertex(41.1F, 5.2F, 0.5F, 100),
                            c = vertex(38.6F, 44.9F, 0.5F, 100), d = vertex(1.4F, 40.3F, 0.5F, 100);
    clear();
    const r3d_span_target_t first = target_at(0);
    const r3d_span_target_t second = target_at(1);
    r3d_span_textured_triangle(&first, &a, &b, &c, uv, texture);
    r3d_span_textured_triangle(&second, &a, &c, &d, uv, texture);
    bool* covered = calloc(PIXELS, sizeof *covered);
    TEST_ASSERT_NOT_NULL(covered);
    int overlap = 0;
    int textured = 0;
    for (int i = 0; i < PIXELS; i++) {
        const bool one = buffers[PIXELS + i] != R3D_DEPTH_EMPTY;
        const bool two = buffers[(3 * PIXELS) + i] != R3D_DEPTH_EMPTY;
        overlap += one && two;
        covered[i] = one || two;
        textured += covered[i];
    }
    clear();
    const r3d_span_target_t plain = target_at(0);
    r3d_span_triangle(&plain, &a, &b, &c);
    r3d_span_triangle(&plain, &a, &c, &d);
    int mismatched = 0;
    for (int i = 0; i < PIXELS; i++) {
        mismatched += covered[i] != (buffers[PIXELS + i] != R3D_DEPTH_EMPTY);
    }
    free(covered);
    TEST_ASSERT_GREATER_THAN(0, textured);
    TEST_ASSERT_EQUAL(0, overlap);
    TEST_ASSERT_EQUAL(0, mismatched);
}

/* One texel a pixel along x, constant v, under the brightest light: each
 * pixel shows the texel under its centre, its colour index times the mean
 * grey's albedo, within the colormap's last level. */
static void
test_affine_samples_the_texel_under_each_centre(void) {
    fixture();
    const r3d_span_target_t target = target_at(0);
    const float grey = 255.0F;
    const r3d_span_vertex_t a = vertex(0, 0, 0.5F, grey), b = vertex(SIDE, 0, 0.5F, grey),
                            c = vertex(0, SIDE, 0.5F, grey);
    const r3d_span_uv_t uv[3] = {{0, 0.5F}, {SIDE, 0.5F}, {0, 0.5F}};
    clear();
    r3d_span_textured_triangle(&target, &a, &b, &c, uv, texture);
    for (int x = 0; x < SIDE / 2; x++) {
        const int index = x & (TEX_SIDE - 1);
        const int expected = index == WHITE_INDEX ? 255 : mathi_min((index * 16 * 255) / R3D_TEXTURE_ONE, 255);
        const uint16_t native = rgb565_swap(buffers[x]);
        TEST_ASSERT_INT_WITHIN(1, expected >> 3, (int)rgb565_r5(native));
        TEST_ASSERT_INT_WITHIN(1, expected >> 2, (int)rgb565_g6(native));
    }
}

/* Four texels a pixel picks level 2, whose texels all hold index 2. */
static void
test_minified_triangle_takes_the_matching_level(void) {
    fixture();
    const r3d_span_target_t target = target_at(0);
    const r3d_span_vertex_t a = vertex(0, 0, 0.5F, 255), b = vertex(SIDE, 0, 0.5F, 255), c = vertex(0, SIDE, 0.5F, 255);
    const r3d_span_uv_t uv[3] = {{0, 0}, {4 * SIDE, 0}, {0, 4 * SIDE}};
    clear();
    r3d_span_textured_triangle(&target, &a, &b, &c, uv, texture);
    const int expected = (2 * 16 * 255) / R3D_TEXTURE_ONE;
    TEST_ASSERT_INT_WITHIN(1, expected >> 2, (int)rgb565_g6(rgb565_swap(buffers[(4 * SIDE) + 4])));
}

/* A texel at the material's mean (MEAN_INDEX) draws the vertex
 * colour itself: averaged over a 4x4 dither tile, within one level. */
static int
tile_green(void) {
    int sum = 0;
    for (int y = 4; y < 8; y++) {
        for (int x = 4; x < 8; x++) {
            sum += (int)rgb565_g6(rgb565_swap(buffers[(y * SIDE) + x]));
        }
    }
    return sum;
}

static void
test_texel_at_the_mean_draws_the_vertex_colour(void) {
    fixture();
    const r3d_span_target_t target = target_at(0);
    const float grey = 100.0F;
    const r3d_span_vertex_t a = vertex(0, 0, 0.5F, grey), b = vertex(SIDE, 0, 0.5F, grey),
                            c = vertex(0, SIDE, 0.5F, grey);
    /* Texel (7, 1) holds MEAN_INDEX, whose colour is R3D_TEXTURE_ONE. */
    const r3d_span_uv_t uv[3] = {{7.5F, 1.5F}, {7.5F, 1.5F}, {7.5F, 1.5F}};
    clear();
    _Static_assert(MEAN_INDEX * 16 == R3D_TEXTURE_ONE, "MEAN_INDEX's colour is the mean");
    r3d_span_textured_triangle(&target, &a, &b, &c, uv, texture);
    const int tile = BAYER4_SIDE * BAYER4_SIDE;
    const int expected = ((int)grey * (int)RGB565_G6_MAX / UINT8_MAX) * tile;
    const int level_g6 = ((int)RGB565_G6_MAX / (R3D_TEXTURE_LIGHT_LEVELS - 1)) + 1;
    TEST_ASSERT_INT_WITHIN(level_g6 * tile, expected, tile_green());
}

static void
test_entry_rejects_bad_headers(void) {
    fixture();
    r3d_texture_t* t = texture;
    const asset_view_t view = {ltex, TEX_BYTES};
    ltex[R3D_TEXTURE_AT_FORMAT] = 2;
    TEST_ASSERT_EQUAL(ASSET_ERR_FORMAT, r3d_texture_from_asset(&view, t));
    write_ltex(ltex);
    ltex[R3D_TEXTURE_AT_LEVELS] = TEX_LOG2 + 1;
    TEST_ASSERT_EQUAL(ASSET_ERR_FORMAT, r3d_texture_from_asset(&view, t));
    write_ltex(ltex);
    const asset_view_t short_view = {ltex, TEX_BYTES - 1};
    TEST_ASSERT_EQUAL(ASSET_ERR_BOUNDS, r3d_texture_from_asset(&short_view, t));
    TEST_ASSERT_NULL(t->texels[0]);
    ltex[R3D_TEXTURE_AT_VERSION] = 2;
    TEST_ASSERT_EQUAL(ASSET_ERR_VERSION, r3d_texture_from_asset(&view, t));
}

void
run_r3d_texture_suite(void) {
    RUN_TEST(test_textured_covers_what_plain_covers);
    RUN_TEST(test_shared_edge_neither_cracks_nor_overlaps);
    RUN_TEST(test_affine_samples_the_texel_under_each_centre);
    RUN_TEST(test_minified_triangle_takes_the_matching_level);
    RUN_TEST(test_texel_at_the_mean_draws_the_vertex_colour);
    RUN_TEST(test_entry_rejects_bad_headers);
}

SUITE_REGISTER(run_r3d_texture_suite);
#endif
