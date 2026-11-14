#ifndef DEVICE_BUILD
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "asset/asset_bytes.h"
#include "asset/asset_pack.h"
#include "collections/bits.h"
#include "core/memory.h"
#include "image/pixel/bayer.h"
#include "image/pixel/rgb565.h"
#include "math/scalar/mathi.h"
#include "math/scalar/rng.h"
#include "r3d_pixel_lens.h"
#include "render/lod/r3d_lod.h"
#include "render/r3d_pipeline.h"
#include "render/r3d_span.h"
#include "render/r3d_span_internal.h"
#include "render/texture/r3d_texture.h"
#include "suites.h"
#include "test_alloc.h"
#include "test_pack.h"
#include "unity.h"

#define SIDE         48
#define PIXELS       (SIDE * SIDE)
#define TARGETS      3 /* each a colour and a depth buffer */
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
static uint8_t* mask_ltex;          /* a MASK1 entry, when a test builds one */
static r3d_texture_t* mask_texture; /* ... and what it opens to */
static bool* marks;                 /* the pixels mark_won() saw won, SIDE a row */
static uint8_t* spare;              /* a test's scratch bytes */

static void
cleanup(void) {
    free(buffers);
    free(ltex);
    free(texture);
    free(mask_ltex);
    free(mask_texture);
    free(marks);
    free(spare);
    buffers = NULL;
    ltex = NULL;
    texture = NULL;
    mask_ltex = NULL;
    mask_texture = NULL;
    marks = NULL;
    spare = NULL;
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
    buffers = calloc((size_t)PIXELS * 2 * TARGETS, sizeof *buffers);
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
    memset(buffers, 0, sizeof(*buffers) * PIXELS * 2 * TARGETS);
}

static r3d_span_vertex_t
vertex(float x, float y, float z, float grey) {
    return (r3d_span_vertex_t){r3d_span_snap(x), r3d_span_snap(y), z, grey, grey, grey};
}

/* The triangle over the target's top-left half, every corner `grey`, drawn
 * into target `which` with the corners' `uv`. */
static void
draw_corner(int which, float grey, const vec2f_t uv[3]) {
    const r3d_span_target_t target = target_at(which);
    const r3d_span_vertex_t a = vertex(0, 0, 0.5F, grey), b = vertex(SIDE, 0, 0.5F, grey),
                            c = vertex(0, SIDE, 0.5F, grey);
    r3d_span_textured_triangle(&target, &a, &b, &c, uv, texture);
}

static void
random_triangle(rng_t* random, r3d_span_vertex_t v[3], vec2f_t uv[3]) {
    for (int k = 0; k < 3; k++) {
        v[k] = (r3d_span_vertex_t){(int32_t)rng_below(random, (SIDE + 16) * R3D_SUBPIXEL) - (8 * R3D_SUBPIXEL),
                                   (int32_t)rng_below(random, (SIDE + 16) * R3D_SUBPIXEL) - (8 * R3D_SUBPIXEL),
                                   NEAR_DEPTH + ((float)rng_below(random, DEPTH_SPREAD) / 4096.0F),
                                   (float)rng_below(random, 256),
                                   (float)rng_below(random, 256),
                                   (float)rng_below(random, 256)};
        uv[k] = (vec2f_t){(float)rng_below(random, 64) - 32.0F, (float)rng_below(random, 64) - 32.0F};
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
        vec2f_t uv[3];
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
    const vec2f_t uv[3] = {{0, 0}, {8, 0}, {8, 8}};
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
    const float grey = 255.0F;
    const vec2f_t uv[3] = {{0, 0.5F}, {SIDE, 0.5F}, {0, 0.5F}};
    clear();
    draw_corner(0, grey, uv);
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
    const vec2f_t uv[3] = {{0, 0}, {4 * SIDE, 0}, {0, 4 * SIDE}};
    clear();
    draw_corner(0, UINT8_MAX, uv);
    const int expected = (2 * 16 * 255) / R3D_TEXTURE_ONE;
    TEST_ASSERT_INT_WITHIN(1, expected >> 2, (int)rgb565_g6(rgb565_swap(buffers[(4 * SIDE) + 4])));
}

/* Moved texels draw what the entry's did, and the entry is no longer read:
 * wiped after the move, the triangle still draws the same pixels. */
static void
test_moved_texels_draw_without_the_entry(void) {
    fixture();
    const vec2f_t uv[3] = {{0, 0}, {SIDE, 0}, {0, SIDE}};
    clear();
    draw_corner(0, UINT8_MAX, uv);
    const size_t bytes = r3d_texture_texel_bytes(texture);
    uint8_t* moved = malloc(bytes);
    TEST_ASSERT_NOT_NULL(moved);
    TEST_ASSERT_EQUAL_PTR(moved + bytes, r3d_texture_move_texels(texture, moved));
    memset(ltex, 0, TEX_BYTES);
    draw_corner(1, UINT8_MAX, uv);
    free(moved);
    TEST_ASSERT_EQUAL_UINT16_ARRAY(buffers, buffers + (2 * PIXELS), 2 * PIXELS);
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
    const float grey = 100.0F;
    /* Texel (7, 1) holds MEAN_INDEX, whose colour is R3D_TEXTURE_ONE. */
    const vec2f_t uv[3] = {{7.5F, 1.5F}, {7.5F, 1.5F}, {7.5F, 1.5F}};
    clear();
    _Static_assert(MEAN_INDEX * 16 == R3D_TEXTURE_ONE, "MEAN_INDEX's colour is the mean");
    draw_corner(0, grey, uv);
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
    ltex[R3D_TEXTURE_AT_FORMAT] = R3D_TEXTURE_MASK1 + 1;
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

#define MASK_LOG2          4
#define MASK_SIDE          (1 << MASK_LOG2)
#define MASK_X_STEP        5 /* texel (x, y) is kept when (5x + 3y) % 7 < 4: unlike its transpose */
#define MASK_Y_STEP        3
#define MASK_MODULUS       7
#define MASK_KEPT_BELOW    4
#define MASK_PALETTE_BYTES (R3D_TEXTURE_MASK1_COLOURS * R3D_TEXTURE_PALETTE_STRIDE)
#define MASK_BIG_LOG2      5 /* a two-level texture: 32 texels a side, then 16 */
#define BG_COLOUR          0x1234
#define BG_DEPTH           100 /* far behind any drawn pixel */
#define BRIGHTEST          255.0F
#define COVERED_MIN        100  /* pixels a test needs covered to tell its draws apart */
#define MASK_LEVEL_1_STEP  2.5F /* texels a pixel steps: level 1 at 2 to under 4 */
#define MASK_FILL_STEP     7    /* texel byte i of a moved texture holds i * 7 + 1 */
#define MASK_GUARD         0xA5
#define FACE_COLOUR_LEFT   0x07E0 /* a face colour a scene triangle draws, panel-order green */
#define FACE_COLOUR_RIGHT  0x001F

/* A MASK1 entry sized to fit exactly: the header, a palette whose colour 1 is
 * the mean, and zeroed texels of `levels` levels from a side of 1 << `log2`.
 * With `palette_last` the texels come first and the palette ends the entry.
 * Sets `*size`. */
static uint8_t*
mask_entry(int log2, int levels, bool palette_last, uint32_t* size) {
    uint32_t texel_bytes = 0;
    for (int l = 0; l < levels; l++) {
        texel_bytes += r3d_texture_level_bytes(R3D_TEXTURE_MASK1, log2 - l);
    }
    *size = R3D_TEXTURE_HEADER_SIZE + MASK_PALETTE_BYTES + texel_bytes;
    mask_ltex = calloc(1, *size);
    suite_set_test_cleanup(cleanup);
    TEST_ASSERT_NOT_NULL(mask_ltex);
    const uint32_t palette_off = palette_last ? R3D_TEXTURE_HEADER_SIZE + texel_bytes : R3D_TEXTURE_HEADER_SIZE;
    const uint32_t texels_off = palette_last ? R3D_TEXTURE_HEADER_SIZE : R3D_TEXTURE_HEADER_SIZE + MASK_PALETTE_BYTES;
    put_u16(mask_ltex + R3D_TEXTURE_AT_VERSION, R3D_TEXTURE_VERSION);
    put_u16(mask_ltex + R3D_TEXTURE_AT_FORMAT, R3D_TEXTURE_MASK1);
    mask_ltex[R3D_TEXTURE_AT_SIZE_LOG2] = (uint8_t)log2;
    mask_ltex[R3D_TEXTURE_AT_LEVELS] = (uint8_t)levels;
    memset(mask_ltex + R3D_TEXTURE_AT_TINT, TINT_GREY, R3D_TEXTURE_TINT_RGB);
    put_u32(mask_ltex + R3D_TEXTURE_AT_PALETTE_OFF, palette_off);
    put_u32(mask_ltex + R3D_TEXTURE_AT_TEXELS_OFF, texels_off);
    memset(mask_ltex + palette_off + R3D_TEXTURE_PALETTE_STRIDE, R3D_TEXTURE_ONE, R3D_TEXTURE_TINT_RGB);
    return mask_ltex;
}

static asset_status_t
mask_open(uint32_t size) {
    mask_texture = malloc(sizeof *mask_texture);
    TEST_ASSERT_NOT_NULL(mask_texture);
    const asset_view_t view = {mask_ltex, size};
    return r3d_texture_from_asset(&view, mask_texture);
}

static void
test_mask1_entry_opens_with_its_format_levels_and_texels(void) {
    uint32_t size;
    (void)mask_entry(MASK_BIG_LOG2, 2, false, &size);
    TEST_ASSERT_EQUAL(ASSET_OK, mask_open(size));
    const uint32_t first = (1U << (2 * MASK_BIG_LOG2)) / R3D_TEXTURE_MASK1_PER_BYTE;
    const uint32_t second = (1U << (2 * (MASK_BIG_LOG2 - 1))) / R3D_TEXTURE_MASK1_PER_BYTE;
    TEST_ASSERT_EQUAL_UINT8(R3D_TEXTURE_MASK1, mask_texture->format);
    TEST_ASSERT_EQUAL_UINT8(MASK_BIG_LOG2, mask_texture->size_log2);
    TEST_ASSERT_EQUAL_UINT8(2, mask_texture->levels);
    TEST_ASSERT_EQUAL_PTR(mask_ltex + R3D_TEXTURE_HEADER_SIZE + MASK_PALETTE_BYTES, mask_texture->texels[0]);
    TEST_ASSERT_EQUAL_PTR(mask_texture->texels[0] + first, mask_texture->texels[1]);
    TEST_ASSERT_EQUAL_UINT32(first + second, (uint32_t)r3d_texture_texel_bytes(mask_texture));
    TEST_ASSERT_EQUAL_INT(R3D_TEXTURE_MASK1_COLOURS, r3d_texture_colours(R3D_TEXTURE_MASK1));
}

/* A side of 3 is a whole INDEX4 level but under MASK1's 4, where a level's
 * rows stop being whole bytes; 4 opens. */
static void
test_mask1_level_0_side_below_4_is_refused(void) {
    uint32_t size;
    (void)mask_entry(R3D_TEXTURE_MASK1_SIZE_LOG2_MIN, 1, false, &size);
    TEST_ASSERT_EQUAL(ASSET_OK, mask_open(size));
    cleanup();
    (void)mask_entry(R3D_TEXTURE_MASK1_SIZE_LOG2_MIN - 1, 1, false, &size);
    TEST_ASSERT_EQUAL(ASSET_ERR_FORMAT, mask_open(size));
    TEST_ASSERT_NULL(mask_texture->texels[0]);
}

/* Level 0 is 16 wide; a third level is 4, a fourth 2. */
static void
test_mask1_last_level_below_4_is_refused(void) {
    uint32_t size;
    const int levels_to_4 = MASK_LOG2 - R3D_TEXTURE_MASK1_SIZE_LOG2_MIN + 1;
    (void)mask_entry(MASK_LOG2, levels_to_4, false, &size);
    TEST_ASSERT_EQUAL(ASSET_OK, mask_open(size));
    cleanup();
    (void)mask_entry(MASK_LOG2, levels_to_4 + 1, false, &size);
    TEST_ASSERT_EQUAL(ASSET_ERR_FORMAT, mask_open(size));
}

/* Texels last, so the entry ends where they do: its exact size opens, one
 * byte less refuses. MASK1's 8 texels a byte size it, not INDEX4's 2. */
static void
test_mask1_texels_running_one_byte_past_the_entry_are_refused(void) {
    uint32_t size;
    (void)mask_entry(MASK_LOG2, 1, false, &size);
    TEST_ASSERT_EQUAL(ASSET_OK, mask_open(size));
    const asset_view_t short_view = {mask_ltex, size - 1};
    TEST_ASSERT_EQUAL(ASSET_ERR_BOUNDS, r3d_texture_from_asset(&short_view, mask_texture));
    TEST_ASSERT_NULL(mask_texture->texels[0]);
}

/* Palette last: the entry's final bytes are exactly the 2 colours, 8 bytes
 * where a 16-colour check wants 64. */
static void
test_mask1_palette_of_two_entries_fitting_exactly_opens(void) {
    uint32_t size;
    (void)mask_entry(MASK_LOG2, 1, true, &size);
    TEST_ASSERT_EQUAL(ASSET_OK, mask_open(size));
    TEST_ASSERT_EQUAL_UINT8(R3D_TEXTURE_MASK1, mask_texture->format);
    const asset_view_t short_view = {mask_ltex, size - 1};
    TEST_ASSERT_EQUAL(ASSET_ERR_BOUNDS, r3d_texture_from_asset(&short_view, mask_texture));
}

static bool
mask_kept(int x, int y) {
    return ((x * MASK_X_STEP) + (y * MASK_Y_STEP)) % MASK_MODULUS < MASK_KEPT_BELOW;
}

/* The MASK1 texture of one MASK_SIDE level whose texel (x, y) is mask_kept(),
 * opened into mask_texture; returns its entry's size. */
static uint32_t
mask_texture_open(void) {
    uint32_t size;
    uint8_t* b = mask_entry(MASK_LOG2, 1, false, &size);
    uint8_t* texels = b + R3D_TEXTURE_HEADER_SIZE + MASK_PALETTE_BYTES;
    for (int y = 0; y < MASK_SIDE; y++) {
        for (int x = 0; x < MASK_SIDE; x++) {
            const int o = (y * MASK_SIDE) + x;
            if (mask_kept(x, y)) {
                bits8_set(texels, (size_t)o);
            }
        }
    }
    TEST_ASSERT_EQUAL(ASSET_OK, mask_open(size));
    return size;
}

/* Colour and depth of every pixel of target `which` as a draw found them. */
static void
background(int which) {
    for (int i = 0; i < PIXELS; i++) {
        buffers[(which * 2 * PIXELS) + i] = BG_COLOUR;
        buffers[(which * 2 * PIXELS) + PIXELS + i] = BG_DEPTH;
    }
}

/* The corner triangle drawn plain, into target `which`. */
static void
plain_corner(int which) {
    const r3d_span_target_t plain = target_at(which);
    const r3d_span_vertex_t a = vertex(0, 0, 0.5F, BRIGHTEST), b = vertex(SIDE, 0, 0.5F, BRIGHTEST),
                            c = vertex(0, SIDE, 0.5F, BRIGHTEST);
    r3d_span_triangle(&plain, &a, &b, &c);
}

/* The corner triangle of draw_corner(), filled with `tex`, uv in pixels so a
 * texel is a pixel. */
static void
draw_masked_stepping(const r3d_span_target_t* target, float texels_a_pixel) {
    const float reach = texels_a_pixel * SIDE;
    const vec2f_t uv[3] = {{0, 0}, {reach, 0}, {0, reach}};
    const r3d_span_vertex_t a = vertex(0, 0, 0.5F, BRIGHTEST), b = vertex(SIDE, 0, 0.5F, BRIGHTEST),
                            c = vertex(0, SIDE, 0.5F, BRIGHTEST);
    r3d_span_textured_triangle(target, &a, &b, &c, uv, mask_texture);
}

static void
draw_masked(const r3d_span_target_t* target) {
    draw_masked_stepping(target, 1.0F);
}

/* Over a pixel a texel, a hole keeps its pixel's colour and depth; a kept
 * texel writes the plain fill's depth and colour 1 at the lit level. The
 * pixels it changes are the plain triangle's, those the pattern keeps. */
static void
test_mask1_holes_keep_the_pixel_and_kept_texels_draw_colour_1(void) {
    fixture();
    mask_texture_open();
    background(0);
    const r3d_span_target_t masked = target_at(0);
    draw_masked(&masked);
    plain_corner(1);
    const uint16_t lit = mask_texture->colormap[R3D_TEXTURE_LIGHT_LEVELS - 1][1];
    TEST_ASSERT_NOT_EQUAL(BG_COLOUR, lit);
    int kept = 0;
    int holes = 0;
    for (int y = 0; y < SIDE; y++) {
        for (int x = 0; x < SIDE; x++) {
            const int i = (y * SIDE) + x;
            const bool covered = buffers[(3 * PIXELS) + i] != R3D_DEPTH_EMPTY;
            if (covered && mask_kept(x % MASK_SIDE, y % MASK_SIDE)) {
                kept++;
                TEST_ASSERT_EQUAL_HEX16_MESSAGE(buffers[(3 * PIXELS) + i], buffers[PIXELS + i], "a kept pixel's depth");
                TEST_ASSERT_EQUAL_HEX16_MESSAGE(lit, buffers[i], "a kept pixel's colour");
            } else {
                holes += covered;
                TEST_ASSERT_EQUAL_HEX16_MESSAGE(BG_DEPTH, buffers[PIXELS + i], "a pixel left as it was: depth");
                TEST_ASSERT_EQUAL_HEX16_MESSAGE(BG_COLOUR, buffers[i], "a pixel left as it was: colour");
            }
        }
    }
    TEST_ASSERT_GREATER_THAN_MESSAGE(0, kept, "no pixel kept: the pattern tells nothing");
    TEST_ASSERT_GREATER_THAN_MESSAGE(0, holes, "no covered pixel cut: the pattern tells nothing");
}

/* A writer that notes each pixel whose depth is the span's own: the pixels
 * the fill won. */
static void
mark_won(const r3d_span_writer_t* writer, const gfx_target_t* rows, int y, int x_first, int x_last, int32_t z,
         int32_t dz) {
    (void)writer;
    const uint16_t* depth = gfx_target_depth(rows, y);
    for (int x = x_first; x <= x_last; x++, z += dz) {
        if ((uint16_t)(z >> R3D_DEPTH_SHIFT) == depth[x]) {
            marks[(y * SIDE) + x] = true;
        }
    }
}

static void
with_marking_writer(r3d_span_target_t* target, r3d_span_writer_t* writer) {
    marks = calloc(PIXELS, sizeof *marks);
    TEST_ASSERT_NOT_NULL(marks);
    *writer = (r3d_span_writer_t){.span = mark_won, .step = R3D_SPAN_STEP_NONE};
    target->writers = writer;
    target->writer_count = 1;
}

static int
marked_pixels(void) {
    int n = 0;
    for (int i = 0; i < PIXELS; i++) {
        n += marks[i];
    }
    return n;
}

/* The writer follows an INDEX4 span: it marks the pixels whose depth the fill
 * changed, all of the triangle and no other. */
static void
test_writers_follow_an_index4_textured_triangle_over_what_it_filled(void) {
    fixture();
    background(0);
    r3d_span_target_t target = target_at(0);
    r3d_span_writer_t writer;
    with_marking_writer(&target, &writer);
    const vec2f_t uv[3] = {{0, 0}, {SIDE, 0}, {0, SIDE}};
    const r3d_span_vertex_t a = vertex(0, 0, 0.5F, BRIGHTEST), b = vertex(SIDE, 0, 0.5F, BRIGHTEST),
                            c = vertex(0, SIDE, 0.5F, BRIGHTEST);
    r3d_span_textured_triangle(&target, &a, &b, &c, uv, texture);
    for (int i = 0; i < PIXELS; i++) {
        TEST_ASSERT_EQUAL_MESSAGE(buffers[PIXELS + i] != BG_DEPTH, marks[i], "a pixel's mark against the fill");
    }
    TEST_ASSERT_GREATER_THAN(PIXELS / 4, marked_pixels());
}

/* The writer follows a MASK1 span over only what it drew: the kept pixels,
 * none of the holes, which keep the depth they had. */
static void
test_writers_follow_a_mask1_triangle_over_its_kept_pixels_only(void) {
    fixture();
    mask_texture_open();
    background(0);
    r3d_span_target_t target = target_at(0);
    r3d_span_writer_t writer;
    with_marking_writer(&target, &writer);
    draw_masked(&target);
    plain_corner(1);
    int covered = 0;
    for (int y = 0; y < SIDE; y++) {
        for (int x = 0; x < SIDE; x++) {
            const int i = (y * SIDE) + x;
            const bool in_triangle = buffers[(3 * PIXELS) + i] != R3D_DEPTH_EMPTY;
            covered += in_triangle;
            TEST_ASSERT_EQUAL_MESSAGE(in_triangle && mask_kept(x % MASK_SIDE, y % MASK_SIDE), marks[i],
                                      "a pixel's mark against the pattern");
        }
    }
    TEST_ASSERT_GREATER_THAN_MESSAGE(0, marked_pixels(), "nothing marked");
    TEST_ASSERT_LESS_THAN_MESSAGE(covered, marked_pixels(), "the holes were marked");
}

/* Where the pixel's stored depth already equals the card's quantised depth, a
 * hole is still not handed to a writer: the plain fill leaves the very depths
 * the card then writes, and only the kept covered pixels are marked. */
static void
test_writers_skip_mask1_holes_where_the_depth_ties(void) {
    fixture();
    mask_texture_open();
    background(0);
    plain_corner(0);
    r3d_span_target_t target = target_at(0);
    r3d_span_writer_t writer;
    with_marking_writer(&target, &writer);
    draw_masked(&target);
    int kept = 0;
    int holes = 0;
    for (int y = 0; y < SIDE; y++) {
        for (int x = 0; x < SIDE; x++) {
            const int i = (y * SIDE) + x;
            const bool in_triangle = buffers[PIXELS + i] != BG_DEPTH;
            const bool is_kept = mask_kept(x % MASK_SIDE, y % MASK_SIDE);
            kept += in_triangle && is_kept;
            holes += in_triangle && !is_kept;
            TEST_ASSERT_EQUAL_MESSAGE(in_triangle && is_kept, marks[i], "a pixel's mark against the pattern");
        }
    }
    TEST_ASSERT_GREATER_THAN_MESSAGE(0, kept, "no kept pixel: the pattern tells nothing");
    TEST_ASSERT_GREATER_THAN_MESSAGE(0, holes, "no covered hole: the pattern tells nothing");
}

/* Depth falling along x and rising down y, a triangle reaching past every
 * side of the target so its spans are clipped: the writer's depth step and
 * each run's first depth must be the fill's own, for the marks to match what
 * the fill wrote. */
#define SLOPE_NEAR  0.25F
#define SLOPE_FAR   0.75F
#define SLOPE_REACH 16

static void
sloped_triangle_marks_what_it_wrote(const r3d_texture_t* tex) {
    background(0);
    r3d_span_target_t target = target_at(0);
    r3d_span_writer_t writer;
    with_marking_writer(&target, &writer);
    const r3d_span_vertex_t a = vertex(-SLOPE_REACH, -SLOPE_REACH, SLOPE_NEAR, BRIGHTEST),
                            b = vertex(SIDE + SLOPE_REACH, 0, SLOPE_FAR, BRIGHTEST),
                            c = vertex(0, SIDE + SLOPE_REACH, SLOPE_FAR, BRIGHTEST);
    const vec2f_t uv[3] = {{0, 0}, {SIDE, 0}, {0, SIDE}};
    r3d_span_textured_triangle(&target, &a, &b, &c, uv, tex);
    int drawn = 0;
    for (int i = 0; i < PIXELS; i++) {
        const bool wrote = buffers[PIXELS + i] != BG_DEPTH;
        drawn += wrote;
        TEST_ASSERT_EQUAL_MESSAGE(wrote, marks[i], "a pixel's mark against what the fill wrote");
    }
    TEST_ASSERT_GREATER_THAN_MESSAGE(PIXELS / 8, drawn, "too little drawn: the slope tells nothing");
}

static void
test_writers_follow_a_sloped_clipped_index4_triangle(void) {
    fixture();
    sloped_triangle_marks_what_it_wrote(texture);
}

static void
test_writers_follow_a_sloped_clipped_mask1_triangle(void) {
    fixture();
    mask_texture_open();
    sloped_triangle_marks_what_it_wrote(mask_texture);
}

static uint16_t
depth_of(int which, int i) {
    return buffers[(which * 2 * PIXELS) + PIXELS + i];
}

/* At a texel a pixel each pixel shows the texel at (x, y): the pattern is
 * not its own transpose, and no covered pixel may follow (y, x). */
static void
test_mask1_pixel_shows_the_pattern_at_x_y_not_y_x(void) {
    fixture();
    mask_texture_open();
    background(0);
    const r3d_span_target_t masked = target_at(0);
    draw_masked(&masked);
    plain_corner(1);
    int told_apart = 0;
    for (int y = 0; y < SIDE; y++) {
        for (int x = 0; x < SIDE; x++) {
            if (depth_of(1, (y * SIDE) + x) == R3D_DEPTH_EMPTY) {
                continue;
            }
            const int u = x % MASK_SIDE;
            const int v = y % MASK_SIDE;
            TEST_ASSERT_EQUAL_MESSAGE(mask_kept(u, v), depth_of(0, (y * SIDE) + x) != BG_DEPTH,
                                      "a pixel against the texel at (x, y)");
            told_apart += mask_kept(u, v) != mask_kept(v, u);
        }
    }
    TEST_ASSERT_GREATER_THAN_MESSAGE(COVERED_MIN, told_apart, "the pattern is too like its transpose to tell");
}

/* Level 0 keeps every texel, level 1 cuts every one: a pixel a texel draws
 * the whole triangle, two and a half draw none, as the level it steps picks. */
static void
test_mask1_each_level_draws_its_own_texels(void) {
    fixture();
    uint32_t size;
    uint8_t* b = mask_entry(MASK_LOG2, 2, false, &size);
    memset(b + R3D_TEXTURE_HEADER_SIZE + MASK_PALETTE_BYTES, UINT8_MAX,
           (MASK_SIDE * MASK_SIDE) / R3D_TEXTURE_MASK1_PER_BYTE);
    TEST_ASSERT_EQUAL(ASSET_OK, mask_open(size));
    background(0);
    background(1);
    const r3d_span_target_t one = target_at(0);
    const r3d_span_target_t two_and_a_half = target_at(1);
    draw_masked_stepping(&one, 1.0F);
    draw_masked_stepping(&two_and_a_half, MASK_LEVEL_1_STEP);
    plain_corner(2);
    int covered = 0;
    for (int i = 0; i < PIXELS; i++) {
        const bool in_triangle = depth_of(2, i) != R3D_DEPTH_EMPTY;
        covered += in_triangle;
        TEST_ASSERT_EQUAL_MESSAGE(in_triangle, depth_of(0, i) != BG_DEPTH, "at a texel a pixel: level 0 keeps all");
        TEST_ASSERT_EQUAL_MESSAGE(false, depth_of(1, i) != BG_DEPTH, "at 2.5 texels a pixel: level 1 cuts all");
    }
    TEST_ASSERT_GREATER_THAN_MESSAGE(COVERED_MIN, covered, "the triangle covers too little to tell");
}

/* A kept texel draws palette colour 1 at the light level, its channels the
 * tint times the palette's, scaled to the tint's luminance. */
static void
test_mask1_kept_colour_is_palette_colour_1_lit_by_the_tint(void) {
    fixture();
    uint32_t size;
    uint8_t* b = mask_entry(MASK_LOG2, 1, false, &size);
    const uint8_t tint[R3D_TEXTURE_TINT_RGB] = {200, 120, 40};
    const uint8_t albedo[R3D_TEXTURE_TINT_RGB] = {64, 128, 192};
    memcpy(b + R3D_TEXTURE_AT_TINT, tint, sizeof tint);
    memcpy(b + R3D_TEXTURE_HEADER_SIZE + R3D_TEXTURE_PALETTE_STRIDE, albedo, sizeof albedo);
    memset(b + R3D_TEXTURE_HEADER_SIZE + MASK_PALETTE_BYTES, UINT8_MAX,
           (MASK_SIDE * MASK_SIDE) / R3D_TEXTURE_MASK1_PER_BYTE);
    TEST_ASSERT_EQUAL(ASSET_OK, mask_open(size));
    const int tint_luma =
        ((R3D_TEXTURE_LUMA_R * tint[0]) + (R3D_TEXTURE_LUMA_G * tint[1]) + (R3D_TEXTURE_LUMA_B * tint[2]))
        >> R3D_TEXTURE_LUMA_SHIFT;
    const int levels[] = {0, R3D_TEXTURE_LIGHT_LEVELS / 2, R3D_TEXTURE_LIGHT_LEVELS - 1};
    for (size_t k = 0; k < sizeof levels / sizeof levels[0]; k++) {
        int want[R3D_TEXTURE_TINT_RGB];
        for (int c = 0; c < (int)R3D_TEXTURE_TINT_RGB; c++) {
            want[c] = mathi_min((tint[c] * albedo[c] * levels[k] * UINT8_MAX)
                                    / (R3D_TEXTURE_ONE * (R3D_TEXTURE_LIGHT_LEVELS - 1) * tint_luma),
                                UINT8_MAX);
        }
        const uint16_t native = rgb565_swap(mask_texture->colormap[levels[k]][1]);
        TEST_ASSERT_INT_WITHIN_MESSAGE(1, want[0] >> 3, (int)rgb565_r5(native), "red of colour 1");
        TEST_ASSERT_INT_WITHIN_MESSAGE(1, want[1] >> 2, (int)rgb565_g6(native), "green of colour 1");
        TEST_ASSERT_INT_WITHIN_MESSAGE(1, want[2] >> 3, (int)rgb565_b5(native), "blue of colour 1");
    }
    background(0);
    const r3d_span_target_t target = target_at(0);
    draw_masked(&target);
    int drawn = 0;
    for (int i = 0; i < PIXELS; i++) {
        if (depth_of(0, i) != BG_DEPTH) {
            drawn++;
            TEST_ASSERT_EQUAL_HEX16(mask_texture->colormap[R3D_TEXTURE_LIGHT_LEVELS - 1][1], buffers[i]);
        }
    }
    TEST_ASSERT_GREATER_THAN(COVERED_MIN, drawn);
}

/* Each level's texels are copied whole, back to back, and the texture points
 * at the copy. */
static void
test_mask1_moved_texels_copy_every_level_exactly(void) {
    uint32_t size;
    uint8_t* b = mask_entry(MASK_BIG_LOG2, 2, false, &size);
    uint8_t* texels = b + R3D_TEXTURE_HEADER_SIZE + MASK_PALETTE_BYTES;
    const size_t first = ((size_t)1 << (2 * MASK_BIG_LOG2)) / R3D_TEXTURE_MASK1_PER_BYTE;
    const size_t total = first + (first / 4);
    for (size_t i = 0; i < total; i++) {
        texels[i] = (uint8_t)((i * MASK_FILL_STEP) + 1);
    }
    TEST_ASSERT_EQUAL(ASSET_OK, mask_open(size));
    TEST_ASSERT_EQUAL_UINT32(total, (uint32_t)r3d_texture_texel_bytes(mask_texture));
    spare = malloc((2 * total) + 1);
    TEST_ASSERT_NOT_NULL(spare);
    memcpy(spare, texels, total);
    uint8_t* moved = spare + total;
    moved[total] = MASK_GUARD;
    TEST_ASSERT_EQUAL_PTR(moved + total, r3d_texture_move_texels(mask_texture, moved));
    TEST_ASSERT_EQUAL_PTR(moved, mask_texture->texels[0]);
    TEST_ASSERT_EQUAL_PTR(moved + first, mask_texture->texels[1]);
    TEST_ASSERT_EQUAL_MEMORY(spare, moved, total);
    TEST_ASSERT_EQUAL_UINT8(MASK_GUARD, moved[total]);
}

/* A mesh's texturing: the LUVS entry, the pack holding its LTEX sibling, and
 * a scene of flat triangles seen at their own pixels, drawn through the
 * pipeline. */
#define MESH_ID           "m"
#define LUVS_BYTES_MAX    160
#define PACK_BYTES        1024
#define UV_PAIR_BYTES     (2 * sizeof(int16_t))
#define READ_VERTICES     4
#define READ_UVS          6 /* the vertices', then the second cluster's two on chart 1 */
#define READ_CLUSTERS     2
#define READ_TRIANGLES    10 /* three bytes of triangle codes, the last one part full */
#define READ_LAST         9  /* the second cluster's last triangle, the one on a chart */
#define READ_CHART_OFFSET 2  /* the second cluster's first vertex, 2, reads chart 1 at 4 */
#define UV_U_UNITS        3  /* 1/16 texel */
#define UV_V_UNITS        5
#define SCENE_VERTICES    3
#define SCENE_TRIANGLES   2
#define SCENE_LIGHT       200
#define SCENE_Z           2.0F
#define BEHIND_Z          (-2.0F)
#define UV_SPAN_UNITS     (TEX_SIDE << R3D_TEXTURE_UV_SHIFT) /* one texture side along a triangle's legs */
#define LEFT_COLUMNS      (SIDE / 2)
#define DISTINCT_MORE     3

typedef struct {
    int16_t positions[SCENE_VERTICES * SCENE_TRIANGLES][3];
    uint8_t colors[SCENE_VERTICES * SCENE_TRIANGLES][3];
    uint16_t triangles[SCENE_TRIANGLES][3];
    int16_t uvs[R3D_TEXTURE_CHARTS * SCENE_VERTICES * SCENE_TRIANGLES][2]; /* chart 0's, then chart 1's */
    r3d_lit_cluster_t cluster;
    r3d_lit_node_t node;
    r3d_pipeline_vertex_t cs[SCENE_VERTICES * SCENE_TRIANGLES];
    r3d_pipeline_rows_t rows[1];
    r3d_lit_mesh_t mesh;
    r3d_texture_t material[R3D_TEXTURE_CHARTS]; /* one a chart at most, as a mesh here has no more */
    r3d_texture_t* decoy;                       /* R3D_TEXTURE_NONE + 1 textures, for a draw that wrongly reads one */
    uint16_t face_colors[SCENE_TRIANGLES];
    r3d_mesh_texturing_t texturing;
    uint8_t luvs[LUVS_BYTES_MAX];
    uint8_t* pack_base;
    void* pack_raw;
    asset_pack_t pack;
    r3d_pipeline_work_t* work;
} scene_t;

static scene_t* scene;

static void
scene_cleanup(void) {
    if (scene != NULL) {
        free(scene->work);
        if (scene->decoy != NULL) {
            memory_free(scene->decoy);
        }
        test_free_aligned(scene->pack_raw);
    }
    free(scene);
    scene = NULL;
    cleanup();
}

/* An LUVS entry's sections, each as the reader exposes it. */
typedef struct {
    uint32_t vertices, uv_count, clusters, triangles;
    const int16_t (*uvs)[2];
    const int32_t* shifts;
    const uint8_t (*materials)[R3D_TEXTURE_CHARTS];
    const uint8_t* codes;
    uint16_t material_count; /* 0 reads as one */
} luvs_parts_t;

/* The LUVS entry of one material: the layout docs/assets/formats/LUVS.md
 * gives, the sections packed one after another; returns its size. */
static uint32_t
write_luvs(uint8_t* b, const luvs_parts_t* in) {
    const uint32_t uvs_off = R3D_TEXTURE_UVS_HEADER_SIZE;
    const uint32_t shifts_off = uvs_off + (in->uv_count * UV_PAIR_BYTES);
    const uint32_t clusters_off = shifts_off + (in->clusters * sizeof(int32_t));
    const uint32_t triangles_off = clusters_off + (in->clusters * R3D_TEXTURE_CHARTS);
    const uint32_t code_bytes =
        (in->triangles + R3D_TEXTURE_UVS_TRIANGLES_PER_BYTE - 1U) / R3D_TEXTURE_UVS_TRIANGLES_PER_BYTE;
    memset(b, 0, LUVS_BYTES_MAX);
    put_u16(b + R3D_TEXTURE_UVS_AT_VERSION, R3D_TEXTURE_UVS_VERSION);
    put_u16(b + R3D_TEXTURE_UVS_AT_MATERIAL_COUNT, in->material_count != 0 ? in->material_count : 1);
    put_u32(b + R3D_TEXTURE_UVS_AT_VERTEX_COUNT, in->vertices);
    put_u32(b + R3D_TEXTURE_UVS_AT_CLUSTER_COUNT, in->clusters);
    put_u32(b + R3D_TEXTURE_UVS_AT_TRIANGLE_COUNT, in->triangles);
    put_u32(b + R3D_TEXTURE_UVS_AT_UV_COUNT, in->uv_count);
    put_u32(b + R3D_TEXTURE_UVS_AT_UVS_OFF, uvs_off);
    put_u32(b + R3D_TEXTURE_UVS_AT_SHIFTS_OFF, shifts_off);
    put_u32(b + R3D_TEXTURE_UVS_AT_CLUSTERS_OFF, clusters_off);
    put_u32(b + R3D_TEXTURE_UVS_AT_TRIANGLES_OFF, triangles_off);
    memcpy(b + uvs_off, in->uvs, in->uv_count * UV_PAIR_BYTES);
    memcpy(b + shifts_off, in->shifts, in->clusters * sizeof(int32_t));
    memcpy(b + clusters_off, in->materials, in->clusters * R3D_TEXTURE_CHARTS);
    memcpy(b + triangles_off, in->codes, code_bytes);
    return triangles_off + code_bytes;
}

/* Triangle `t`'s `code` placed in a code byte. */
static uint8_t
code_at(int t, unsigned code) {
    return (uint8_t)(code << (((unsigned)t % R3D_TEXTURE_UVS_TRIANGLES_PER_BYTE) * R3D_TEXTURE_UVS_CODE_BITS));
}

/* The scene and a pack of the one LTEX the mesh "m" names. */
static scene_t*
scene_fixture_of(const uint8_t* entry, uint32_t entry_bytes, const uint8_t* second, uint32_t second_bytes) {
    scene = calloc(1, sizeof *scene);
    suite_set_test_cleanup(scene_cleanup);
    TEST_ASSERT_NOT_NULL(scene);
    scene->work = malloc(r3d_pipeline_work_bytes());
    TEST_ASSERT_NOT_NULL(scene->work);
    scene->pack_base = test_alloc_aligned(PACK_BYTES, ASSET_PACK_BASE_ALIGN, &scene->pack_raw);
    TEST_ASSERT_NOT_NULL(scene->pack_base);
    test_pack_t built = test_pack_begin(scene->pack_base, PACK_BYTES, second == NULL ? 1 : 2);
    memcpy(test_pack_add(&built, MESH_ID R3D_TEXTURE_MATERIAL_SUFFIX "0", R3D_TEXTURE_ASSET, entry_bytes), entry,
           entry_bytes);
    if (second != NULL) {
        memcpy(test_pack_add(&built, MESH_ID R3D_TEXTURE_MATERIAL_SUFFIX "1", R3D_TEXTURE_ASSET, second_bytes), second,
               second_bytes);
    }
    const uint32_t total = test_pack_finish(&built);
    TEST_ASSERT_EQUAL(ASSET_OK, asset_pack_open(&scene->pack, scene->pack_base, total));
    return scene;
}

static scene_t*
scene_fixture(void) {
    fixture();
    return scene_fixture_of(ltex, TEX_BYTES, NULL, 0);
}

/* The scene whose material 0 is the INDEX4 texture and material 1 the MASK1
 * one of mask_texture_open(). */
static scene_t*
scene_fixture_pair(void) {
    fixture();
    const uint32_t size = mask_texture_open();
    return scene_fixture_of(ltex, TEX_BYTES, mask_ltex, size);
}

/* The same scene whose material is the MASK1 texture of mask_texture_open(). */
static scene_t*
scene_fixture_mask(void) {
    fixture();
    const uint32_t size = mask_texture_open();
    return scene_fixture_of(mask_ltex, size, NULL, 0);
}

static asset_status_t
open_texturing(scene_t* s, uint32_t size, const r3d_lit_mesh_t* mesh) {
    const asset_view_t view = {s->luvs, size};
    return r3d_texture_texturing_from_asset(&view, &s->pack, MESH_ID, mesh, s->material, R3D_TEXTURE_CHARTS,
                                            &s->texturing);
}

static void
corner(scene_t* s, int k, float x, float y, float z) {
    s->positions[k][0] = (int16_t)(x * R3D_SUBPIXEL * z);
    s->positions[k][1] = (int16_t)(y * R3D_SUBPIXEL * z);
    s->positions[k][2] = (int16_t)z;
    memset(s->colors[k], SCENE_LIGHT, sizeof s->colors[k]);
}

/* The mesh's UVs: on chart 0 the texture's first side along each
 * triangle's legs, on chart 1 `chart_1_sides` of them. */
static void
leg_uvs(scene_t* s, int triangles, int chart_1_sides) {
    for (int t = 0; t < triangles; t++) {
        for (int chart = 0; chart < R3D_TEXTURE_CHARTS; chart++) {
            const int16_t span = (int16_t)(chart == 0 ? UV_SPAN_UNITS : UV_SPAN_UNITS * chart_1_sides);
            const int16_t uv[SCENE_VERTICES][2] = {{0, 0}, {span, 0}, {0, span}};
            memcpy(s->uvs[(chart * triangles * SCENE_VERTICES) + (t * SCENE_VERTICES)], uv, sizeof uv);
        }
        for (int k = 0; k < SCENE_VERTICES; k++) {
            s->triangles[t][k] = (uint16_t)((t * SCENE_VERTICES) + k);
        }
    }
}

/* Builds the one-cluster mesh of `triangles` triangles whose corners are set,
 * with its texturing read from an LUVS entry of triangle codes `codes`; the
 * cluster's charts name the materials `chart_materials`, of `material_count`. */
static void
scene_mesh_with(scene_t* s, int triangles, uint8_t codes, const uint8_t chart_materials[R3D_TEXTURE_CHARTS],
                uint16_t material_count) {
    const int vertices = triangles * SCENE_VERTICES;
    const box3s_t reach = {{-INT16_MAX, -INT16_MAX, -8}, {INT16_MAX, INT16_MAX, 8}};
    s->cluster = (r3d_lit_cluster_t){0, (uint16_t)vertices, 0, (uint16_t)triangles, reach, true};
    s->node = (r3d_lit_node_t){reach, 0, 1, true};
    s->mesh = (r3d_lit_mesh_t){.positions = s->positions,
                               .colors = s->colors,
                               .triangles = s->triangles,
                               .clusters = &s->cluster,
                               .nodes = &s->node,
                               .vertex_count = vertices,
                               .triangle_count = triangles,
                               .cluster_count = 1,
                               .node_count = 1,
                               .position_scale = 1};
    const int32_t shifts[1] = {vertices};
    uint8_t materials[1][R3D_TEXTURE_CHARTS];
    memcpy(materials[0], chart_materials, R3D_TEXTURE_CHARTS);
    const luvs_parts_t parts = {(uint32_t)vertices,
                                (uint32_t)(R3D_TEXTURE_CHARTS * vertices),
                                1,
                                (uint32_t)triangles,
                                s->uvs,
                                shifts,
                                materials,
                                &codes,
                                material_count};
    const uint32_t size = write_luvs(s->luvs, &parts);
    TEST_ASSERT_EQUAL(ASSET_OK, open_texturing(s, size, &s->mesh));
    s->mesh.texturing = &s->texturing;
}

static void
scene_mesh(scene_t* s, int triangles, uint8_t codes, int charts) {
    const uint8_t chart_materials[R3D_TEXTURE_CHARTS] = {0, charts > 1 ? 0 : R3D_TEXTURE_NONE};
    scene_mesh_with(s, triangles, codes, chart_materials, 1);
}

/* Draws the scene into target `which`, with its texturing or plain. */
static void
draw_scene_into(scene_t* s, const r3d_span_target_t* target, bool textured) {
    const r3d_lens_t lens = r3d_pixel_lens(SIDE, SIDE);
    const uint16_t visible[1] = {0};
    r3d_pipeline_transform(&s->mesh, &lens, visible, 1, s->cs, s->rows, false);
    s->mesh.texturing = textured ? &s->texturing : NULL;
    r3d_pipeline_draw(&s->mesh, &lens, visible, 1, s->cs, s->rows, target, s->work);
}

static void
draw_scene(scene_t* s, int which, bool textured) {
    const r3d_span_target_t target = target_at(which);
    draw_scene_into(s, &target, textured);
}

static const uint16_t*
colours_of(int which) {
    return buffers + (which * 2 * PIXELS);
}

static const uint16_t*
depths_of(int which) {
    return colours_of(which) + PIXELS;
}

static int
distinct_colours(int which) {
    bool* seen = calloc((size_t)UINT16_MAX + 1, sizeof *seen);
    TEST_ASSERT_NOT_NULL(seen);
    int n = 0;
    for (int i = 0; i < PIXELS; i++) {
        const uint16_t c = colours_of(which)[i];
        if (depths_of(which)[i] != R3D_DEPTH_EMPTY && !seen[c]) {
            seen[c] = true;
            n++;
        }
    }
    free(seen);
    return n;
}

/* An entry for READ_VERTICES vertices in READ_CLUSTERS clusters and
 * READ_TRIANGLES triangles: the first cluster, vertices 0-1 and triangles
 * 0-4, on chart 0 alone; the second, vertices 2-3 and triangles 5-9, on two
 * charts; every triangle plain but READ_LAST, on the second cluster's chart
 * 1. */
static const r3d_lit_cluster_t read_clusters[READ_CLUSTERS] = {
    {.vertex_first = 0, .vertex_count = 2, .triangle_first = 0, .triangle_count = 5},
    {.vertex_first = 2, .vertex_count = 2, .triangle_first = 5, .triangle_count = 5}};

static uint32_t
write_read_entry(scene_t* s) {
    const int16_t uvs[READ_UVS][2] = {{0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {UV_U_UNITS, UV_V_UNITS}};
    const int32_t shifts[READ_CLUSTERS] = {0, READ_CHART_OFFSET};
    const uint8_t materials[READ_CLUSTERS][R3D_TEXTURE_CHARTS] = {{0, R3D_TEXTURE_NONE}, {0, 0}};
    uint8_t codes[3] = {0};
    codes[READ_LAST / R3D_TEXTURE_UVS_TRIANGLES_PER_BYTE] = code_at(READ_LAST, 2);
    const luvs_parts_t parts = {READ_VERTICES, READ_UVS, READ_CLUSTERS, READ_TRIANGLES, uvs, shifts, materials,
                                codes,         0};
    return write_luvs(s->luvs, &parts);
}

static r3d_lit_mesh_t
read_mesh(void) {
    return (r3d_lit_mesh_t){.clusters = read_clusters,
                            .vertex_count = READ_VERTICES,
                            .cluster_count = READ_CLUSTERS,
                            .triangle_count = READ_TRIANGLES};
}

static void
test_luvs_entry_opens_and_exposes_its_codes_and_uvs(void) {
    scene_t* s = scene_fixture();
    const r3d_lit_mesh_t mesh = read_mesh();
    const uint32_t size = write_read_entry(s);
    TEST_ASSERT_EQUAL(ASSET_OK, open_texturing(s, size, &mesh));
    TEST_ASSERT_EQUAL(1, s->texturing.material_count);
    TEST_ASSERT_EQUAL_PTR(s->material, s->texturing.materials);
    TEST_ASSERT_EQUAL_UINT8(0, s->texturing.cluster_material[0][0]);
    TEST_ASSERT_EQUAL_UINT8(R3D_TEXTURE_NONE, s->texturing.cluster_material[0][1]);
    TEST_ASSERT_EQUAL_UINT8(0, s->texturing.cluster_material[1][1]);
    for (int t = 0; t < READ_TRIANGLES; t++) {
        TEST_ASSERT_EQUAL_MESSAGE(t == READ_LAST ? 2U : R3D_TEXTURE_PLAIN, r3d_texture_triangle_code(&s->texturing, t),
                                  "a triangle code");
    }
    /* Chart 1 reads the vertex's UV past its shift, in 1/16 texel; chart 0
     * reads the vertex's own. */
    const vec2f_t on_1 = r3d_texture_uv(&s->texturing, 1, 1, READ_VERTICES - 1);
    TEST_ASSERT_EQUAL_FLOAT((float)UV_U_UNITS / (1 << R3D_TEXTURE_UV_SHIFT), on_1.x);
    TEST_ASSERT_EQUAL_FLOAT((float)UV_V_UNITS / (1 << R3D_TEXTURE_UV_SHIFT), on_1.y);
    const vec2f_t on_0 = r3d_texture_uv(&s->texturing, 1, 0, READ_VERTICES - 1);
    TEST_ASSERT_EQUAL_FLOAT(0.0F, on_0.x);
    TEST_ASSERT_EQUAL_FLOAT(0.0F, on_0.y);
}

static void
expect_refused(scene_t* s, uint32_t size, const r3d_lit_mesh_t* mesh, asset_status_t expected, const char* why) {
    memset(&s->texturing, 0xFF, sizeof s->texturing);
    TEST_ASSERT_EQUAL_MESSAGE(expected, open_texturing(s, size, mesh), why);
    TEST_ASSERT_NULL_MESSAGE(s->texturing.uvs, why);
    TEST_ASSERT_NULL_MESSAGE(s->texturing.cluster_material, why);
    TEST_ASSERT_NULL_MESSAGE(s->texturing.cluster_shift, why);
    TEST_ASSERT_NULL_MESSAGE(s->texturing.triangle_code, why);
    TEST_ASSERT_NULL_MESSAGE(s->texturing.materials, why);
    TEST_ASSERT_EQUAL_MESSAGE(0, s->texturing.material_count, why);
}

static void
test_luvs_entry_is_refused_when_its_counts_do_not_match_the_mesh(void) {
    scene_t* s = scene_fixture();
    const uint32_t size = write_read_entry(s);
    r3d_lit_mesh_t mesh = read_mesh();
    mesh.triangle_count++;
    expect_refused(s, size, &mesh, ASSET_ERR_FORMAT, "triangle count");
    mesh = read_mesh();
    mesh.vertex_count++;
    expect_refused(s, size, &mesh, ASSET_ERR_FORMAT, "vertex count");
    mesh = read_mesh();
    mesh.cluster_count++;
    expect_refused(s, size, &mesh, ASSET_ERR_FORMAT, "cluster count");
}

static void
test_luvs_entry_must_end_exactly_after_its_triangle_codes(void) {
    scene_t* s = scene_fixture();
    const r3d_lit_mesh_t mesh = read_mesh();
    const uint32_t size = write_read_entry(s);
    expect_refused(s, size + 1, &mesh, ASSET_ERR_BOUNDS, "one byte more");
    expect_refused(s, size - 1, &mesh, ASSET_ERR_BOUNDS, "one byte less");
    TEST_ASSERT_EQUAL(ASSET_OK, open_texturing(s, size, &mesh));
}

static void
test_luvs_cluster_material_past_the_count_is_refused(void) {
    scene_t* s = scene_fixture();
    const r3d_lit_mesh_t mesh = read_mesh();
    const uint32_t size = write_read_entry(s);
    const uint32_t clusters_off = asset_read_u32(s->luvs + R3D_TEXTURE_UVS_AT_CLUSTERS_OFF);
    s->luvs[clusters_off + 1] = 1; /* one material: index 1 is past it, NONE is not */
    expect_refused(s, size, &mesh, ASSET_ERR_FORMAT, "material index");
}

/* Where each section of the read entry starts, and the bytes a test edits. */
static uint32_t
header_word(const scene_t* s, uint32_t at) {
    return asset_read_u32(s->luvs + at);
}

static void
test_luvs_code_naming_chart_1_of_a_one_chart_cluster_is_refused(void) {
    scene_t* s = scene_fixture();
    const r3d_lit_mesh_t mesh = read_mesh();
    const uint32_t size = write_read_entry(s);
    s->luvs[header_word(s, R3D_TEXTURE_UVS_AT_TRIANGLES_OFF)] = code_at(0, 2);
    expect_refused(s, size, &mesh, ASSET_ERR_FORMAT, "a code naming chart 1 of a cluster with one chart");
}

static void
test_luvs_chart_1_after_a_chart_with_no_material_is_refused(void) {
    scene_t* s = scene_fixture();
    const r3d_lit_mesh_t mesh = read_mesh();
    const uint32_t size = write_read_entry(s);
    s->luvs[header_word(s, R3D_TEXTURE_UVS_AT_CLUSTERS_OFF) + R3D_TEXTURE_CHARTS] = R3D_TEXTURE_NONE;
    expect_refused(s, size, &mesh, ASSET_ERR_FORMAT, "chart 1 after a chart with no material");
}

static void
test_luvs_chart_1_uvs_past_the_entrys_are_refused(void) {
    scene_t* s = scene_fixture();
    const r3d_lit_mesh_t mesh = read_mesh();
    const uint32_t size = write_read_entry(s);
    put_u32(s->luvs + header_word(s, R3D_TEXTURE_UVS_AT_SHIFTS_OFF) + sizeof(int32_t), READ_CHART_OFFSET + 1);
    expect_refused(s, size, &mesh, ASSET_ERR_FORMAT, "chart 1's UVs one past the entry's");
}

/* The shift that puts the second cluster's chart 1 `moved` UVs from where the
 * entry has them. */
static void
shift_cluster_1(scene_t* s, int32_t moved) {
    put_u32(s->luvs + header_word(s, R3D_TEXTURE_UVS_AT_SHIFTS_OFF) + sizeof(int32_t),
            (uint32_t)(int32_t)(READ_CHART_OFFSET + moved));
}

static void
test_luvs_negative_shift_of_a_two_chart_cluster_is_refused(void) {
    scene_t* s = scene_fixture();
    const r3d_lit_mesh_t mesh = read_mesh();
    const uint32_t size = write_read_entry(s);
    shift_cluster_1(s, -(int32_t)(read_clusters[1].vertex_first + READ_CHART_OFFSET + 1));
    expect_refused(s, size, &mesh, ASSET_ERR_FORMAT, "chart 1's UVs one before the entry's");
}

static void
test_luvs_chart_1_uvs_fitting_the_entry_exactly_at_each_end_open(void) {
    scene_t* s = scene_fixture();
    const r3d_lit_mesh_t mesh = read_mesh();
    const uint32_t size = write_read_entry(s);
    TEST_ASSERT_EQUAL_UINT32(READ_UVS,
                             read_clusters[1].vertex_first + READ_CHART_OFFSET + read_clusters[1].vertex_count);
    TEST_ASSERT_EQUAL(ASSET_OK, open_texturing(s, size, &mesh));
    shift_cluster_1(s, -(int32_t)(read_clusters[1].vertex_first + READ_CHART_OFFSET));
    TEST_ASSERT_EQUAL(ASSET_OK, open_texturing(s, size, &mesh));
}

static void
test_luvs_one_chart_cluster_opens_with_a_shift_out_of_range(void) {
    scene_t* s = scene_fixture();
    const r3d_lit_mesh_t mesh = read_mesh();
    const uint32_t size = write_read_entry(s);
    const int32_t far = 1000;
    put_u32(s->luvs + header_word(s, R3D_TEXTURE_UVS_AT_SHIFTS_OFF), (uint32_t)far);
    TEST_ASSERT_EQUAL(ASSET_OK, open_texturing(s, size, &mesh));
    put_u32(s->luvs + header_word(s, R3D_TEXTURE_UVS_AT_SHIFTS_OFF), (uint32_t)-far);
    TEST_ASSERT_EQUAL(ASSET_OK, open_texturing(s, size, &mesh));
}

/* Triangle `t`'s code byte, set to `code` with its neighbours' bits cleared. */
static void
set_code(scene_t* s, int t, unsigned code) {
    uint8_t* byte =
        s->luvs + header_word(s, R3D_TEXTURE_UVS_AT_TRIANGLES_OFF) + (t / R3D_TEXTURE_UVS_TRIANGLES_PER_BYTE);
    *byte = code_at(t, code);
}

/* The read entry with no triangle on a chart, to set one code in. */
static uint32_t
write_plain_entry(scene_t* s) {
    const uint32_t size = write_read_entry(s);
    for (int t = 0; t < READ_TRIANGLES; t += R3D_TEXTURE_UVS_TRIANGLES_PER_BYTE) {
        set_code(s, t, R3D_TEXTURE_PLAIN);
    }
    return size;
}

static void
test_luvs_code_3_in_a_two_chart_cluster_is_refused(void) {
    scene_t* s = scene_fixture();
    const r3d_lit_mesh_t mesh = read_mesh();
    const uint32_t size = write_plain_entry(s);
    set_code(s, 7, R3D_TEXTURE_CHARTS + 1);
    expect_refused(s, size, &mesh, ASSET_ERR_FORMAT, "code 3 in a cluster with two charts");
}

static void
test_luvs_bad_code_in_the_second_cluster_is_refused(void) {
    scene_t* s = scene_fixture();
    const r3d_lit_mesh_t mesh = read_mesh();
    const uint32_t size = write_plain_entry(s);
    set_code(s, read_clusters[1].triangle_first, R3D_TEXTURE_CHARTS + 1);
    expect_refused(s, size, &mesh, ASSET_ERR_FORMAT, "a bad code in cluster 1");
}

static void
test_luvs_bad_code_on_the_last_partly_packed_triangle_is_refused(void) {
    scene_t* s = scene_fixture();
    const r3d_lit_mesh_t mesh = read_mesh();
    TEST_ASSERT_NOT_EQUAL(0, READ_TRIANGLES % R3D_TEXTURE_UVS_TRIANGLES_PER_BYTE);
    const uint32_t size = write_plain_entry(s);
    set_code(s, READ_TRIANGLES - 1, R3D_TEXTURE_CHARTS + 1);
    expect_refused(s, size, &mesh, ASSET_ERR_FORMAT, "a bad code on the last triangle");
}

static void
test_luvs_codes_up_to_the_clusters_last_chart_open(void) {
    scene_t* s = scene_fixture();
    const r3d_lit_mesh_t mesh = read_mesh();
    const uint32_t size = write_plain_entry(s);
    set_code(s, 0, 1);
    set_code(s, read_clusters[1].triangle_first, R3D_TEXTURE_CHARTS);
    TEST_ASSERT_EQUAL(ASSET_OK, open_texturing(s, size, &mesh));
}

/* The read entry with chart `chart` of cluster `c` naming material `k`. */
static uint32_t
write_naming(scene_t* s, int c, int chart, uint8_t k) {
    const uint32_t size = write_read_entry(s);
    s->luvs[header_word(s, R3D_TEXTURE_UVS_AT_CLUSTERS_OFF) + (c * R3D_TEXTURE_CHARTS) + chart] = k;
    return size;
}

static void
test_luvs_chart_0_naming_the_material_count_is_refused(void) {
    scene_t* s = scene_fixture();
    const r3d_lit_mesh_t mesh = read_mesh();
    const uint32_t size = write_naming(s, 0, 0, 1);
    expect_refused(s, size, &mesh, ASSET_ERR_FORMAT, "chart 0 of cluster 0 naming material 1 of 1");
}

static void
test_luvs_second_clusters_chart_0_naming_the_material_count_is_refused(void) {
    scene_t* s = scene_fixture();
    const r3d_lit_mesh_t mesh = read_mesh();
    const uint32_t size = write_naming(s, 1, 0, 1);
    expect_refused(s, size, &mesh, ASSET_ERR_FORMAT, "chart 0 of cluster 1 naming material 1 of 1");
}

static void
test_luvs_second_clusters_chart_1_naming_the_material_count_is_refused(void) {
    scene_t* s = scene_fixture();
    const r3d_lit_mesh_t mesh = read_mesh();
    const uint32_t size = write_naming(s, 1, 1, 1);
    expect_refused(s, size, &mesh, ASSET_ERR_FORMAT, "chart 1 of cluster 1 naming material 1 of 1");
}

static void
test_luvs_material_one_below_the_count_opens_on_every_chart(void) {
    scene_t* s = scene_fixture();
    const r3d_lit_mesh_t mesh = read_mesh();
    const uint32_t size = write_naming(s, 1, 1, 0);
    TEST_ASSERT_EQUAL(ASSET_OK, open_texturing(s, size, &mesh));
    TEST_ASSERT_EQUAL_UINT8(0, s->texturing.cluster_material[0][0]);
    TEST_ASSERT_EQUAL_UINT8(0, s->texturing.cluster_material[1][0]);
    TEST_ASSERT_EQUAL_UINT8(0, s->texturing.cluster_material[1][1]);
}

static void
test_luvs_with_fewer_uvs_than_vertices_is_refused(void) {
    scene_t* s = scene_fixture();
    const r3d_lit_mesh_t mesh = read_mesh();
    const uint32_t size = write_plain_entry(s);
    s->luvs[header_word(s, R3D_TEXTURE_UVS_AT_CLUSTERS_OFF) + R3D_TEXTURE_CHARTS + 1] = R3D_TEXTURE_NONE;
    TEST_ASSERT_EQUAL(ASSET_OK, open_texturing(s, size, &mesh));
    put_u32(s->luvs + R3D_TEXTURE_UVS_AT_UV_COUNT, READ_VERTICES - 1);
    expect_refused(s, size, &mesh, ASSET_ERR_FORMAT, "one UV fewer than vertices");
}

/* The read entry with the section offset at `at` set to `value`. */
static void
expect_offset_refused(scene_t* s, uint32_t at, uint32_t value, const char* why) {
    const r3d_lit_mesh_t mesh = read_mesh();
    const uint32_t size = write_read_entry(s);
    put_u32(s->luvs + at, value);
    expect_refused(s, size, &mesh, ASSET_ERR_BOUNDS, why);
}

/* Past the written entry, aligned for every section. */
#define PAST_ENTRY 76U

static void
test_luvs_misaligned_uvs_offset_is_refused(void) {
    scene_t* s = scene_fixture();
    (void)write_read_entry(s);
    expect_offset_refused(s, R3D_TEXTURE_UVS_AT_UVS_OFF, header_word(s, R3D_TEXTURE_UVS_AT_UVS_OFF) + 1,
                          "UVs at an odd byte");
}

static void
test_luvs_misaligned_shifts_offset_is_refused(void) {
    scene_t* s = scene_fixture();
    (void)write_read_entry(s);
    expect_offset_refused(s, R3D_TEXTURE_UVS_AT_SHIFTS_OFF, header_word(s, R3D_TEXTURE_UVS_AT_SHIFTS_OFF) + 2,
                          "shifts off a 4-byte boundary");
}

static void
test_luvs_uvs_offset_inside_the_header_is_refused(void) {
    expect_offset_refused(scene_fixture(), R3D_TEXTURE_UVS_AT_UVS_OFF, 0, "UVs over the header");
}

static void
test_luvs_shifts_offset_inside_the_header_is_refused(void) {
    expect_offset_refused(scene_fixture(), R3D_TEXTURE_UVS_AT_SHIFTS_OFF, 0, "shifts over the header");
}

static void
test_luvs_clusters_offset_inside_the_header_is_refused(void) {
    expect_offset_refused(scene_fixture(), R3D_TEXTURE_UVS_AT_CLUSTERS_OFF, 0, "cluster materials over the header");
}

static void
test_luvs_triangles_offset_inside_the_header_is_refused(void) {
    expect_offset_refused(scene_fixture(), R3D_TEXTURE_UVS_AT_TRIANGLES_OFF, 0, "triangle codes over the header");
}

static void
test_luvs_uvs_running_past_the_entry_are_refused(void) {
    expect_offset_refused(scene_fixture(), R3D_TEXTURE_UVS_AT_UVS_OFF, PAST_ENTRY, "UVs past the entry");
}

static void
test_luvs_shifts_running_past_the_entry_are_refused(void) {
    expect_offset_refused(scene_fixture(), R3D_TEXTURE_UVS_AT_SHIFTS_OFF, PAST_ENTRY, "shifts past the entry");
}

static void
test_luvs_cluster_materials_running_past_the_entry_are_refused(void) {
    expect_offset_refused(scene_fixture(), R3D_TEXTURE_UVS_AT_CLUSTERS_OFF, PAST_ENTRY,
                          "cluster materials past the entry");
}

static void
test_luvs_triangle_codes_running_past_the_entry_are_refused(void) {
    expect_offset_refused(scene_fixture(), R3D_TEXTURE_UVS_AT_TRIANGLES_OFF, PAST_ENTRY,
                          "triangle codes past the entry");
}

static void
test_luvs_names_a_material_the_pack_lacks(void) {
    scene_t* s = scene_fixture();
    const r3d_lit_mesh_t mesh = read_mesh();
    const uint32_t size = write_read_entry(s);
    const asset_view_t view = {s->luvs, size};
    memset(&s->texturing, 0xFF, sizeof s->texturing);
    TEST_ASSERT_EQUAL(ASSET_ERR_NOT_FOUND,
                      r3d_texture_texturing_from_asset(&view, &s->pack, "other", &mesh, s->material, R3D_TEXTURE_CHARTS,
                                                       &s->texturing));
    TEST_ASSERT_NULL(s->texturing.uvs);
    TEST_ASSERT_NULL(s->texturing.triangle_code);
}

/* One cluster of two disjoint triangles, the left one in the left half,
 * each drawn as its code in `codes` says; chart 1 maps `chart_1_sides`
 * texture sides along the legs. */
static void
place_two_triangles(scene_t* s, int chart_1_sides) {
    const float left[3][2] = {{2, 4}, {22, 4}, {12, 40}};
    const float right[3][2] = {{26, 4}, {46, 4}, {36, 40}};
    for (int k = 0; k < SCENE_VERTICES; k++) {
        corner(s, k, left[k][0], left[k][1], SCENE_Z);
        corner(s, SCENE_VERTICES + k, right[k][0], right[k][1], SCENE_Z);
    }
    leg_uvs(s, SCENE_TRIANGLES, chart_1_sides);
}

static void
two_triangles(scene_t* s, uint8_t codes, int chart_1_sides) {
    place_two_triangles(s, chart_1_sides);
    scene_mesh(s, SCENE_TRIANGLES, codes, R3D_TEXTURE_CHARTS);
}

/* Pixels of target 1 that differ from target 0's plain draw, in the left
 * half and the right. */
static void
differing(int* left, int* right) {
    *left = 0;
    *right = 0;
    for (int i = 0; i < PIXELS; i++) {
        TEST_ASSERT_EQUAL_MESSAGE(depths_of(0)[i] != R3D_DEPTH_EMPTY, depths_of(1)[i] != R3D_DEPTH_EMPTY, "coverage");
        const bool differs = colours_of(0)[i] != colours_of(1)[i];
        (*((i % SIDE) < LEFT_COLUMNS ? left : right)) += differs;
    }
}

static void
expect_drawn_textured(bool marked, int differing_pixels, const char* which) {
    if (marked) {
        TEST_ASSERT_GREATER_THAN_MESSAGE(0, differing_pixels, which);
    } else {
        TEST_ASSERT_EQUAL_MESSAGE(0, differing_pixels, which);
    }
}

static void
test_only_triangles_on_a_chart_draw_textured(void) {
    const unsigned codes[][SCENE_TRIANGLES] = {
        {1, R3D_TEXTURE_PLAIN}, {R3D_TEXTURE_PLAIN, 2}, {R3D_TEXTURE_PLAIN, R3D_TEXTURE_PLAIN}};
    for (size_t m = 0; m < sizeof codes / sizeof codes[0]; m++) {
        scene_t* s = scene_fixture();
        two_triangles(s, code_at(0, codes[m][0]) | code_at(1, codes[m][1]), 1);
        clear();
        draw_scene(s, 0, false);
        draw_scene(s, 1, true);
        int left, right;
        differing(&left, &right);
        expect_drawn_textured(codes[m][0] != R3D_TEXTURE_PLAIN, left, "the left triangle");
        expect_drawn_textured(codes[m][1] != R3D_TEXTURE_PLAIN, right, "the right triangle");
        scene_cleanup();
    }
}

/* A triangle on chart 1 reads chart 1's UVs: drawn on chart 1 it matches
 * itself drawn on chart 0 with the same UVs, and not chart 0's own. */
static void
test_a_triangle_on_chart_1_draws_with_chart_1s_uvs(void) {
    const int sides = 2;
    scene_t* s = scene_fixture();
    two_triangles(s, code_at(0, 2), sides);
    clear();
    draw_scene(s, 0, true);
    scene_mesh(s, SCENE_TRIANGLES, code_at(0, 1), R3D_TEXTURE_CHARTS);
    draw_scene(s, 1, true);
    for (int k = 0; k < SCENE_TRIANGLES * SCENE_VERTICES; k++) {
        for (int i = 0; i < 2; i++) {
            s->uvs[k][i] = (int16_t)(s->uvs[k][i] * sides);
        }
    }
    scene_mesh(s, SCENE_TRIANGLES, code_at(0, 1), R3D_TEXTURE_CHARTS);
    draw_scene(s, 2, true);
    int differs_from_chart_0 = 0;
    for (int i = 0; i < PIXELS; i++) {
        TEST_ASSERT_EQUAL_HEX16_MESSAGE(colours_of(2)[i], colours_of(0)[i], "chart 1 against the same UVs on chart 0");
        differs_from_chart_0 += colours_of(0)[i] != colours_of(1)[i];
    }
    TEST_ASSERT_GREATER_THAN_MESSAGE(0, differs_from_chart_0, "chart 1 drew chart 0's UVs");
}

static void
test_near_clipped_textured_triangle_keeps_its_coverage_and_its_pattern(void) {
    scene_t* s = scene_fixture();
    /* Two corners in front, one behind the near plane. */
    corner(s, 0, 8, 20, SCENE_Z);
    corner(s, 1, 40, 20, SCENE_Z);
    corner(s, 2, 0, 0, BEHIND_Z);
    leg_uvs(s, 1, 1);
    scene_mesh(s, 1, 1, 1);
    clear();
    draw_scene(s, 0, false);
    draw_scene(s, 1, true);
    int covered = 0;
    int differs = 0;
    for (int i = 0; i < PIXELS; i++) {
        const bool plain = depths_of(0)[i] != R3D_DEPTH_EMPTY;
        TEST_ASSERT_EQUAL_MESSAGE(plain, depths_of(1)[i] != R3D_DEPTH_EMPTY,
                                  "the clipped draws cover different pixels");
        covered += plain;
        differs += colours_of(0)[i] != colours_of(1)[i];
    }
    TEST_ASSERT_GREATER_THAN_MESSAGE(COVERED_MIN, covered, "the clipped triangle covers too little to tell");
    TEST_ASSERT_GREATER_THAN_MESSAGE(covered / 2, differs, "the part in front is not textured");
    TEST_ASSERT_GREATER_THAN_MESSAGE(distinct_colours(0) + DISTINCT_MORE, distinct_colours(1),
                                     "the clipped triangle's UVs do not vary across it");
}

static int
covered_pixels(int which) {
    int n = 0;
    for (int i = 0; i < PIXELS; i++) {
        n += depths_of(which)[i] != R3D_DEPTH_EMPTY;
    }
    return n;
}

/* Both triangles on chart 0, a MASK1 texture, the target with a writer: the
 * mesh draws textured, as it does with no writer, and the writer marks the
 * pixels that draw won, which are fewer than the plain triangles cover. */
static void
test_a_mask1_chart_draws_textured_under_a_writer_marking_only_kept_pixels(void) {
    scene_t* s = scene_fixture_mask();
    two_triangles(s, code_at(0, 1) | code_at(1, 1), 1);
    clear();
    draw_scene(s, 0, false);
    background(1);
    draw_scene(s, 1, true);
    background(2);
    r3d_span_target_t target = target_at(2);
    r3d_span_writer_t writer;
    with_marking_writer(&target, &writer);
    draw_scene_into(s, &target, true);
    const int plain = covered_pixels(0);
    TEST_ASSERT_GREATER_THAN_MESSAGE(COVERED_MIN, plain, "the triangles cover too little to tell");
    int drawn = 0;
    for (int i = 0; i < PIXELS; i++) {
        TEST_ASSERT_EQUAL_HEX16_MESSAGE(colours_of(1)[i], colours_of(2)[i],
                                        "the writer's draw is not the textured one");
        TEST_ASSERT_EQUAL_HEX16_MESSAGE(depths_of(1)[i], depths_of(2)[i], "the writer's draw is not the textured one");
        TEST_ASSERT_EQUAL_MESSAGE(depths_of(1)[i] != BG_DEPTH, marks[i], "a pixel's mark against what was drawn");
        drawn += depths_of(1)[i] != BG_DEPTH;
    }
    TEST_ASSERT_GREATER_THAN_MESSAGE(0, drawn, "nothing drawn");
    TEST_ASSERT_LESS_THAN_MESSAGE(plain, drawn, "no hole cut: it drew plain");
    TEST_ASSERT_EQUAL_INT(drawn, marked_pixels());
}

/* An INDEX4 chart under a writer is drawn as the plain mesh, as before the
 * textured fill took writers: the same colours and depth, the writer marking
 * what the plain draw covers; with no writer it does draw textured. */
static void
test_an_index4_chart_under_a_writer_draws_exactly_as_plain(void) {
    scene_t* s = scene_fixture();
    two_triangles(s, code_at(0, 1) | code_at(1, 1), 1);
    clear();
    r3d_span_target_t plain_target = target_at(0);
    r3d_span_writer_t plain_writer;
    with_marking_writer(&plain_target, &plain_writer);
    draw_scene_into(s, &plain_target, false);
    const int plain_marks = marked_pixels();
    memset(marks, 0, PIXELS * sizeof *marks);
    r3d_span_target_t textured_target = target_at(1);
    textured_target.writers = &plain_writer;
    textured_target.writer_count = 1;
    draw_scene_into(s, &textured_target, true);
    draw_scene(s, 2, true);
    TEST_ASSERT_EQUAL_INT(covered_pixels(0), plain_marks);
    TEST_ASSERT_EQUAL_INT(plain_marks, marked_pixels());
    for (int i = 0; i < PIXELS; i++) {
        TEST_ASSERT_EQUAL_HEX16_MESSAGE(colours_of(0)[i], colours_of(1)[i], "a colour differs from the plain draw");
        TEST_ASSERT_EQUAL_HEX16_MESSAGE(depths_of(0)[i], depths_of(1)[i], "a depth differs from the plain draw");
    }
    int textured_differs = 0;
    for (int i = 0; i < PIXELS; i++) {
        textured_differs += colours_of(2)[i] != colours_of(0)[i];
    }
    TEST_ASSERT_GREATER_THAN_MESSAGE(0, textured_differs,
                                     "the writerless draw is not textured: the test tells nothing");
}

/* Whether target `a` and `b` hold the same colour and depth in columns
 * [from, to). */
static void
expect_columns_equal(int a, int b, int from, int to, const char* why) {
    for (int y = 0; y < SIDE; y++) {
        for (int x = from; x < to; x++) {
            const int i = (y * SIDE) + x;
            TEST_ASSERT_EQUAL_HEX16_MESSAGE(colours_of(a)[i], colours_of(b)[i], why);
            TEST_ASSERT_EQUAL_HEX16_MESSAGE(depths_of(a)[i], depths_of(b)[i], why);
        }
    }
}

/* Pixels of columns [from, to) of target `which` that differ from the background. */
static int
drawn_in_columns(int which, int from, int to) {
    int n = 0;
    for (int y = 0; y < SIDE; y++) {
        for (int x = from; x < to; x++) {
            n += depths_of(which)[(y * SIDE) + x] != BG_DEPTH;
        }
    }
    return n;
}

static int
marked_in_columns(int from, int to) {
    int n = 0;
    for (int y = 0; y < SIDE; y++) {
        for (int x = from; x < to; x++) {
            n += marks[(y * SIDE) + x];
        }
    }
    return n;
}

/* The left triangle on chart 0, an INDEX4 material; the right on chart 1, a
 * MASK1 one. Under a writer the left draws exactly as plain and the right
 * as the textured, cut draw, the writer marking only its kept pixels. */
static void
test_under_a_writer_a_mask1_triangle_is_cut_and_an_index4_one_is_plain(void) {
    scene_t* s = scene_fixture_pair();
    place_two_triangles(s, 1);
    const uint8_t chart_materials[R3D_TEXTURE_CHARTS] = {0, 1};
    scene_mesh_with(s, SCENE_TRIANGLES, (uint8_t)(code_at(0, 1) | code_at(1, 2)), chart_materials, 2);
    background(0);
    draw_scene(s, 0, false);
    background(1);
    draw_scene(s, 1, true);
    background(2);
    r3d_span_target_t target = target_at(2);
    r3d_span_writer_t writer;
    with_marking_writer(&target, &writer);
    draw_scene_into(s, &target, true);
    expect_columns_equal(2, 0, 0, LEFT_COLUMNS, "the INDEX4 triangle is not the plain one");
    expect_columns_equal(2, 1, LEFT_COLUMNS, SIDE, "the MASK1 triangle is not the textured one");
    int left_textured = 0;
    for (int i = 0; i < PIXELS; i++) {
        left_textured += (i % SIDE) < LEFT_COLUMNS && colours_of(1)[i] != colours_of(0)[i];
        TEST_ASSERT_EQUAL_MESSAGE(depths_of(2)[i] != BG_DEPTH, marks[i], "a pixel's mark against what was drawn");
    }
    TEST_ASSERT_GREATER_THAN_MESSAGE(0, left_textured, "the INDEX4 chart is not textured without a writer");
    const int right_plain = drawn_in_columns(0, LEFT_COLUMNS, SIDE);
    TEST_ASSERT_GREATER_THAN_MESSAGE(0, marked_in_columns(LEFT_COLUMNS, SIDE), "the MASK1 triangle drew nothing");
    TEST_ASSERT_LESS_THAN_MESSAGE(right_plain, marked_in_columns(LEFT_COLUMNS, SIDE), "the MASK1 triangle is not cut");
    TEST_ASSERT_EQUAL_INT(drawn_in_columns(0, 0, LEFT_COLUMNS), marked_in_columns(0, LEFT_COLUMNS));
}

/* `s` drawn with a writer, textured, marks and draws what it marks and draws
 * plain: target 0 plain, target 1 textured, `message` when they differ. */
static void
expect_writer_pass_draws_plain(scene_t* s, const char* message) {
    background(0);
    r3d_span_target_t plain = target_at(0);
    r3d_span_writer_t plain_writer;
    with_marking_writer(&plain, &plain_writer);
    draw_scene_into(s, &plain, false);
    const int plain_marks = marked_pixels();
    memset(marks, 0, PIXELS * sizeof *marks);
    background(1);
    r3d_span_target_t textured = target_at(1);
    textured.writers = &plain_writer;
    textured.writer_count = 1;
    draw_scene_into(s, &textured, true);
    TEST_ASSERT_GREATER_THAN_MESSAGE(COVERED_MIN, plain_marks, "the plain draw covers too little to tell");
    TEST_ASSERT_EQUAL_INT(plain_marks, marked_pixels());
    expect_columns_equal(1, 0, 0, SIDE, message);
}

/* A cluster that names no material draws plain even where a triangle's code
 * names a chart: the material it would read is a MASK1 decoy that would cut. */
static void
test_under_a_writer_a_cluster_with_no_material_draws_plain(void) {
    scene_t* s = scene_fixture_pair();
    place_two_triangles(s, 1);
    const uint8_t chart_materials[R3D_TEXTURE_CHARTS] = {0, 1};
    scene_mesh_with(s, SCENE_TRIANGLES, (uint8_t)(code_at(0, 2) | code_at(1, 2)), chart_materials, 2);
    const uint32_t at = header_word(s, R3D_TEXTURE_UVS_AT_CLUSTERS_OFF);
    s->luvs[at] = R3D_TEXTURE_NONE;
    s->luvs[at + 1] = R3D_TEXTURE_NONE;
    s->decoy = memory_alloc(((size_t)R3D_TEXTURE_NONE + 1) * sizeof *s->decoy, MEMORY_PSRAM);
    TEST_ASSERT_NOT_NULL(s->decoy);
    memset(s->decoy, 0, ((size_t)R3D_TEXTURE_NONE + 1) * sizeof *s->decoy);
    s->decoy[R3D_TEXTURE_NONE] = s->material[1];
    s->texturing.materials = s->decoy;
    expect_writer_pass_draws_plain(s, "a cluster with no material did not draw plain");
}

/* A mesh with no vertex colours has nothing to light a texture with: its
 * triangles on a MASK1 chart draw as the plain face-coloured ones. */
static void
test_under_a_writer_a_mesh_with_no_vertex_colours_draws_plain(void) {
    scene_t* s = scene_fixture_pair();
    place_two_triangles(s, 1);
    const uint8_t chart_materials[R3D_TEXTURE_CHARTS] = {0, 1};
    scene_mesh_with(s, SCENE_TRIANGLES, (uint8_t)(code_at(0, 2) | code_at(1, 2)), chart_materials, 2);
    s->mesh.colors = NULL;
    s->face_colors[0] = FACE_COLOUR_LEFT;
    s->face_colors[1] = FACE_COLOUR_RIGHT;
    s->mesh.face_colors = s->face_colors;
    expect_writer_pass_draws_plain(s, "a mesh with no vertex colours did not draw plain");
}

/* A band that draws half of each 4x4 tile, and a morph weight halfway to the
 * targets: a blend that changes a plain draw. */
#define HALF_BAND   ((uint8_t)((R3D_LOD_DITHER_LEVELS / 2) << R3D_LOD_DITHER_SHIFT))
#define HALF_WEIGHT ((uint8_t)(R3D_LOD_MORPH_ONE / 2))

/* `s` drawn textured into target `which` as a cluster in a blend: `blend` its
 * dither band with `dither`, else its morph weight. */
static void
draw_blended_into(scene_t* s, int which, uint8_t blend, bool dither) {
    const r3d_lens_t lens = r3d_pixel_lens(SIDE, SIDE);
    const uint16_t visible[1] = {0};
    const r3d_span_target_t target = target_at(which);
    r3d_pipeline_transform(&s->mesh, &lens, visible, 1, s->cs, s->rows, false);
    s->rows[0].blend = blend;
    s->mesh.texturing = &s->texturing;
    r3d_pipeline_draw_blended(&s->mesh, &lens, visible, 1, s->cs, s->rows, &target, s->work, dither);
}

/* The mixed mesh: the left triangle on an INDEX4 chart, the right on a MASK1
 * one; target 0 drawn plain, target 1 textured, outside a blend. */
static scene_t*
mixed_scene_drawn(void) {
    scene_t* s = scene_fixture_pair();
    place_two_triangles(s, 1);
    const uint8_t chart_materials[R3D_TEXTURE_CHARTS] = {0, 1};
    scene_mesh_with(s, SCENE_TRIANGLES, (uint8_t)(code_at(0, 1) | code_at(1, 2)), chart_materials, 2);
    background(0);
    draw_scene(s, 0, false);
    background(1);
    draw_scene(s, 1, true);
    return s;
}

/* A cut is coverage: in a morph a MASK1 chart draws textured and whole, never
 * as a solid quad; an INDEX4 chart draws plain until the blend ends. */
static void
test_in_a_morph_a_mask1_chart_is_cut_and_an_index4_one_is_plain(void) {
    scene_t* s = mixed_scene_drawn();
    background(2);
    draw_blended_into(s, 2, HALF_WEIGHT, false);
    expect_columns_equal(2, 0, 0, LEFT_COLUMNS, "the INDEX4 triangle in a morph is not the plain one");
    expect_columns_equal(2, 1, LEFT_COLUMNS, SIDE, "the MASK1 triangle in a morph is not the textured one");
    TEST_ASSERT_LESS_THAN_MESSAGE(drawn_in_columns(0, LEFT_COLUMNS, SIDE), drawn_in_columns(2, LEFT_COLUMNS, SIDE),
                                  "the MASK1 triangle in a morph is not cut");
}

/* In a dither band the MASK1 chart still draws whole and cut, its band not
 * applied; the INDEX4 one draws plain through the band, some of its pixels. */
static void
test_in_a_dither_band_a_mask1_chart_draws_whole_and_cut(void) {
    scene_t* s = mixed_scene_drawn();
    background(2);
    draw_blended_into(s, 2, HALF_BAND, true);
    expect_columns_equal(2, 1, LEFT_COLUMNS, SIDE, "the MASK1 triangle in a band is not the whole textured one");
    TEST_ASSERT_LESS_THAN_MESSAGE(drawn_in_columns(0, LEFT_COLUMNS, SIDE), drawn_in_columns(2, LEFT_COLUMNS, SIDE),
                                  "the MASK1 triangle in a band is not cut");
    int banded = 0;
    for (int y = 0; y < SIDE; y++) {
        for (int x = 0; x < LEFT_COLUMNS; x++) {
            const int i = (y * SIDE) + x;
            if (depths_of(2)[i] != BG_DEPTH) {
                TEST_ASSERT_EQUAL_HEX16_MESSAGE(colours_of(0)[i], colours_of(2)[i],
                                                "the INDEX4 triangle in a band is not plain");
                banded++;
            }
        }
    }
    TEST_ASSERT_GREATER_THAN_MESSAGE(0, banded, "the INDEX4 triangle's band drew nothing");
    TEST_ASSERT_LESS_THAN_MESSAGE(drawn_in_columns(0, 0, LEFT_COLUMNS), banded,
                                  "the INDEX4 triangle drew past its band");
}

/* Whether mesh MESH_ID cuts follows its materials' formats; a mesh with no
 * LUVS does not, and a material whose header is malformed is reported. */
static void
test_a_mesh_cuts_when_its_luvs_names_a_mask1_material(void) {
    fixture();
    const uint32_t mask_bytes = mask_texture_open();
    const uint32_t uvs_bytes = R3D_TEXTURE_UVS_HEADER_SIZE;
    for (int material = 0; material <= 2; material++) {
        void* raw;
        uint8_t* base = test_alloc_aligned(PACK_BYTES, ASSET_PACK_BASE_ALIGN, &raw);
        TEST_ASSERT_NOT_NULL(base);
        test_pack_t built = test_pack_begin(base, PACK_BYTES, 2);
        uint8_t* uvs = test_pack_add(&built, MESH_ID R3D_TEXTURE_UVS_NAME_SUFFIX, R3D_TEXTURE_UVS_ASSET, uvs_bytes);
        put_u16(uvs + R3D_TEXTURE_UVS_AT_VERSION, R3D_TEXTURE_UVS_VERSION);
        put_u16(uvs + R3D_TEXTURE_UVS_AT_MATERIAL_COUNT, 1);
        const bool cut = material == 1;
        uint8_t* entry = test_pack_add(&built, MESH_ID R3D_TEXTURE_MATERIAL_SUFFIX "0", R3D_TEXTURE_ASSET,
                                       cut ? mask_bytes : TEX_BYTES);
        memcpy(entry, cut ? mask_ltex : ltex, cut ? mask_bytes : TEX_BYTES);
        if (material == 2) {
            put_u16(entry + R3D_TEXTURE_AT_VERSION, R3D_TEXTURE_VERSION + 1);
        }
        asset_pack_t pack;
        TEST_ASSERT_EQUAL(ASSET_OK, asset_pack_open(&pack, base, test_pack_finish(&built)));
        bool cuts = true;
        TEST_ASSERT_EQUAL(material == 2 ? ASSET_ERR_VERSION : ASSET_OK, r3d_texture_mesh_cuts(&pack, MESH_ID, &cuts));
        TEST_ASSERT_EQUAL(cut, cuts);
        TEST_ASSERT_EQUAL(ASSET_OK, r3d_texture_mesh_cuts(&pack, "other", &cuts));
        TEST_ASSERT_FALSE(cuts);
        test_free_aligned(raw);
    }
}

void
run_r3d_texture_suite(void) {
    RUN_TEST(test_textured_covers_what_plain_covers);
    RUN_TEST(test_shared_edge_neither_cracks_nor_overlaps);
    RUN_TEST(test_affine_samples_the_texel_under_each_centre);
    RUN_TEST(test_minified_triangle_takes_the_matching_level);
    RUN_TEST(test_moved_texels_draw_without_the_entry);
    RUN_TEST(test_texel_at_the_mean_draws_the_vertex_colour);
    RUN_TEST(test_entry_rejects_bad_headers);
    RUN_TEST(test_mask1_entry_opens_with_its_format_levels_and_texels);
    RUN_TEST(test_mask1_level_0_side_below_4_is_refused);
    RUN_TEST(test_mask1_last_level_below_4_is_refused);
    RUN_TEST(test_mask1_texels_running_one_byte_past_the_entry_are_refused);
    RUN_TEST(test_mask1_palette_of_two_entries_fitting_exactly_opens);
    RUN_TEST(test_mask1_holes_keep_the_pixel_and_kept_texels_draw_colour_1);
    RUN_TEST(test_writers_follow_an_index4_textured_triangle_over_what_it_filled);
    RUN_TEST(test_writers_follow_a_mask1_triangle_over_its_kept_pixels_only);
    RUN_TEST(test_writers_skip_mask1_holes_where_the_depth_ties);
    RUN_TEST(test_writers_follow_a_sloped_clipped_index4_triangle);
    RUN_TEST(test_writers_follow_a_sloped_clipped_mask1_triangle);
    RUN_TEST(test_mask1_pixel_shows_the_pattern_at_x_y_not_y_x);
    RUN_TEST(test_mask1_each_level_draws_its_own_texels);
    RUN_TEST(test_mask1_kept_colour_is_palette_colour_1_lit_by_the_tint);
    RUN_TEST(test_mask1_moved_texels_copy_every_level_exactly);
    RUN_TEST(test_luvs_entry_opens_and_exposes_its_codes_and_uvs);
    RUN_TEST(test_luvs_entry_is_refused_when_its_counts_do_not_match_the_mesh);
    RUN_TEST(test_luvs_entry_must_end_exactly_after_its_triangle_codes);
    RUN_TEST(test_luvs_cluster_material_past_the_count_is_refused);
    RUN_TEST(test_luvs_code_naming_chart_1_of_a_one_chart_cluster_is_refused);
    RUN_TEST(test_luvs_chart_1_after_a_chart_with_no_material_is_refused);
    RUN_TEST(test_luvs_chart_1_uvs_past_the_entrys_are_refused);
    RUN_TEST(test_luvs_negative_shift_of_a_two_chart_cluster_is_refused);
    RUN_TEST(test_luvs_chart_1_uvs_fitting_the_entry_exactly_at_each_end_open);
    RUN_TEST(test_luvs_one_chart_cluster_opens_with_a_shift_out_of_range);
    RUN_TEST(test_luvs_code_3_in_a_two_chart_cluster_is_refused);
    RUN_TEST(test_luvs_bad_code_in_the_second_cluster_is_refused);
    RUN_TEST(test_luvs_bad_code_on_the_last_partly_packed_triangle_is_refused);
    RUN_TEST(test_luvs_codes_up_to_the_clusters_last_chart_open);
    RUN_TEST(test_luvs_chart_0_naming_the_material_count_is_refused);
    RUN_TEST(test_luvs_second_clusters_chart_0_naming_the_material_count_is_refused);
    RUN_TEST(test_luvs_second_clusters_chart_1_naming_the_material_count_is_refused);
    RUN_TEST(test_luvs_material_one_below_the_count_opens_on_every_chart);
    RUN_TEST(test_luvs_with_fewer_uvs_than_vertices_is_refused);
    RUN_TEST(test_luvs_misaligned_uvs_offset_is_refused);
    RUN_TEST(test_luvs_misaligned_shifts_offset_is_refused);
    RUN_TEST(test_luvs_uvs_offset_inside_the_header_is_refused);
    RUN_TEST(test_luvs_shifts_offset_inside_the_header_is_refused);
    RUN_TEST(test_luvs_clusters_offset_inside_the_header_is_refused);
    RUN_TEST(test_luvs_triangles_offset_inside_the_header_is_refused);
    RUN_TEST(test_luvs_uvs_running_past_the_entry_are_refused);
    RUN_TEST(test_luvs_shifts_running_past_the_entry_are_refused);
    RUN_TEST(test_luvs_cluster_materials_running_past_the_entry_are_refused);
    RUN_TEST(test_luvs_triangle_codes_running_past_the_entry_are_refused);
    RUN_TEST(test_luvs_names_a_material_the_pack_lacks);
    RUN_TEST(test_only_triangles_on_a_chart_draw_textured);
    RUN_TEST(test_a_triangle_on_chart_1_draws_with_chart_1s_uvs);
    RUN_TEST(test_near_clipped_textured_triangle_keeps_its_coverage_and_its_pattern);
    RUN_TEST(test_a_mask1_chart_draws_textured_under_a_writer_marking_only_kept_pixels);
    RUN_TEST(test_an_index4_chart_under_a_writer_draws_exactly_as_plain);
    RUN_TEST(test_under_a_writer_a_mask1_triangle_is_cut_and_an_index4_one_is_plain);
    RUN_TEST(test_under_a_writer_a_cluster_with_no_material_draws_plain);
    RUN_TEST(test_under_a_writer_a_mesh_with_no_vertex_colours_draws_plain);
    RUN_TEST(test_in_a_morph_a_mask1_chart_is_cut_and_an_index4_one_is_plain);
    RUN_TEST(test_in_a_dither_band_a_mask1_chart_draws_whole_and_cut);
    RUN_TEST(test_a_mesh_cuts_when_its_luvs_names_a_mask1_material);
}

SUITE_REGISTER(run_r3d_texture_suite);
#endif
