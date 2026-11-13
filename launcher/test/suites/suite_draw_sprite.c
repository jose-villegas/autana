/*
 * Portable suite: draw/draw_sprite. Opening a SPRT or IMAG entry and every
 * check it makes, against entries built here; on the host, the nine-slice
 * blit's geometry at several sizes and all four turns, its coverage, its flat
 * centre and its clip; on both, the test tile in its pack drawn from the pack
 * and from RAM alike.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "suites.h"
#include "unity.h"

#include "asset/asset_store.h"
#include "draw/draw.h"
#include "draw/draw_image_format.h"
#include "draw/draw_sprite.h"
#include "gfx/gfx.h"
#include "gfx/present/gfx_present.h"
#include "gfx/present/gfx_test.h"
#include "image/pixel/rgb565.h"
#include "test_alloc.h"
#include "test_pack.h"
#include "testkit/cleanup.h"

#define SPRITE_TEST_PACK "sprite_test"
#define SPRITE_TEST_TILE "tile"
/* The test tile's border and its centre's alpha, as tile.image.toml and tile.png make them. */
#define TILE_BORDER      16
#define TILE_CENTRE      128

/* A 5 x 4 RGB565A8 picture in a SPRT with a border of 1 all round. */
enum {
    PIC_W = 5,
    PIC_H = 4,
    PIC_STRIDE = 8, /* PIC_W rounded up to DRAW_IMAGE_RGB565A8_STRIDE_STEP */
    PIC_BORDER = 1,
    IMAG_BYTES = DRAW_IMAGE_HEADER_SIZE + (PIC_STRIDE * PIC_H * ((int)sizeof(gfx_color_t) + 1)),
    SPRT_BYTES = DRAW_SPRITE_HEADER_SIZE + IMAG_BYTES,
    SPRT_LAST_ALPHA_END = SPRT_BYTES - (PIC_STRIDE - PIC_W), /* the last row's padding is not needed */
    PACK_BYTES = 1024,
    TEST_ALIGN = 16,
};

typedef struct {
    uint8_t* bytes;
    void* raw;
    asset_pack_t pack;
} built_pack_t;

static void
put_imag(uint8_t* at, int format, int stride) {
    test_pack_put16(at, DRAW_IMAGE_VERSION);
    test_pack_put16(at + DRAW_IMAGE_AT_FORMAT, format);
    test_pack_put16(at + DRAW_IMAGE_AT_WIDTH, PIC_W);
    test_pack_put16(at + DRAW_IMAGE_AT_HEIGHT, PIC_H);
    test_pack_put32(at + DRAW_IMAGE_AT_STRIDE, (uint32_t)stride);
    test_pack_put32(at + DRAW_IMAGE_AT_PIXELS, DRAW_IMAGE_HEADER_SIZE);
}

/* A pack holding "pic", a SPRT whose u8 at `at` is `value` (`at` < 0 for
 * none), entry size `size`; "bare", the same picture as an IMAG; "mono", a
 * one-bit IMAG in a SPRT; "other", neither type. */
static built_pack_t
build_pack(int at, int value, uint32_t size) {
    built_pack_t built;
    built.bytes = test_alloc_aligned(PACK_BYTES, TEST_ALIGN, &built.raw);
    TEST_ASSERT_NOT_NULL(built.bytes);
    memset(built.bytes, 0, PACK_BYTES);
    test_pack_t pack = test_pack_begin(built.bytes, PACK_BYTES, 4);
    uint8_t* sprite = test_pack_add(&pack, "pic", DRAW_SPRITE_ASSET, size);
    uint8_t header[SPRT_BYTES] = {0};
    test_pack_put16(header, DRAW_SPRITE_VERSION);
    memset(header + DRAW_SPRITE_AT_BORDER, PIC_BORDER, 4);
    put_imag(header + DRAW_SPRITE_HEADER_SIZE, DRAW_IMAGE_FORMAT_RGB565A8, PIC_STRIDE);
    if (at >= 0) {
        header[at] = (uint8_t)value;
    }
    memcpy(sprite, header, size < SPRT_BYTES ? size : SPRT_BYTES);
    put_imag(test_pack_add(&pack, "bare", DRAW_IMAGE_ASSET, IMAG_BYTES), DRAW_IMAGE_FORMAT_RGB565A8, PIC_STRIDE);
    uint8_t* mono = test_pack_add(&pack, "mono", DRAW_SPRITE_ASSET, SPRT_BYTES);
    test_pack_put16(mono, DRAW_SPRITE_VERSION);
    put_imag(mono + DRAW_SPRITE_HEADER_SIZE, DRAW_IMAGE_FORMAT_MONO1, DRAW_IMAGE_MONO1_STRIDE_STEP);
    (void)test_pack_add(&pack, "other", ASSET_TYPE('T', 'A', 'B', 'L'), 4);
    TEST_ASSERT_EQUAL_INT(ASSET_OK, asset_pack_open(&built.pack, built.bytes, test_pack_finish(&pack)));
    return built;
}

/* The status opening `name` gives; a failure must leave the sprite zeroed. */
static asset_status_t
open_status(int at, int value, uint32_t size, const char* name) {
    built_pack_t built = build_pack(at, value, size);
    sprite_t sprite;
    const asset_status_t status = draw_sprite_open(&built.pack, name, &sprite);
    test_free_aligned(built.raw);
    const sprite_t zero = {0};
    TEST_ASSERT_TRUE_MESSAGE(status == ASSET_OK || memcmp(&sprite, &zero, sizeof sprite) == 0,
                             "a refused sprite was left behind");
    return status;
}

static void
test_a_sprite_opens_its_border_and_both_planes_in_place(void) {
    built_pack_t built = build_pack(DRAW_SPRITE_AT_BORDER + 2, 2, SPRT_BYTES);
    sprite_t s;
    const asset_status_t status = draw_sprite_open(&built.pack, "pic", &s);
    asset_view_t entry;
    TEST_ASSERT_EQUAL_INT(ASSET_OK, asset_pack_find(&built.pack, "pic", DRAW_SPRITE_ASSET, &entry));
    const bool in_place =
        (const uint8_t*)s.color.pixels == (const uint8_t*)entry.data + SPRT_BYTES - IMAG_BYTES + DRAW_IMAGE_HEADER_SIZE;
    test_free_aligned(built.raw);
    TEST_ASSERT_EQUAL_INT(ASSET_OK, status);
    TEST_ASSERT_TRUE_MESSAGE(in_place, "the colour plane was not the entry's own bytes");
    TEST_ASSERT_EQUAL_UINT8(PIC_BORDER, s.border.left);
    TEST_ASSERT_EQUAL_UINT8(2, s.border.right);
    TEST_ASSERT_NOT_NULL(s.alpha.pixels);
    TEST_ASSERT_EQUAL_UINT16(PIC_W, s.color.width);
}

static void
test_a_bare_image_opens_as_a_sprite_with_no_border(void) {
    built_pack_t built = build_pack(-1, 0, SPRT_BYTES);
    sprite_t s;
    const asset_status_t status = draw_sprite_open(&built.pack, "bare", &s);
    test_free_aligned(built.raw);
    TEST_ASSERT_EQUAL_INT(ASSET_OK, status);
    TEST_ASSERT_EQUAL_UINT8(0, s.border.left + s.border.top + s.border.right + s.border.bottom);
    TEST_ASSERT_FALSE(s.flat_centre);
}

static void
test_each_bad_sprite_header_value_is_refused(void) {
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_VERSION, open_status(0, DRAW_SPRITE_VERSION + 1, SPRT_BYTES, "pic"));
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_FORMAT, open_status(DRAW_SPRITE_AT_FLAT, 2, SPRT_BYTES, "pic"));
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_FORMAT, open_status(DRAW_SPRITE_AT_RESERVED, 1, SPRT_BYTES, "pic"));
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_BOUNDS, open_status(-1, 0, DRAW_SPRITE_HEADER_SIZE - 1, "pic"));
    TEST_ASSERT_EQUAL_INT(ASSET_OK, open_status(-1, 0, SPRT_LAST_ALPHA_END, "pic"));
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_BOUNDS, open_status(-1, 0, SPRT_LAST_ALPHA_END - 1, "pic"));
}

static void
test_a_border_leaving_no_centre_column_or_row_is_out_of_bounds(void) {
    const int left = DRAW_SPRITE_AT_BORDER;
    const int top = DRAW_SPRITE_AT_BORDER + 1;
    TEST_ASSERT_EQUAL_INT(ASSET_OK, open_status(left, PIC_W - PIC_BORDER - 1, SPRT_BYTES, "pic"));
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_BOUNDS, open_status(left, PIC_W - PIC_BORDER, SPRT_BYTES, "pic"));
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_BOUNDS, open_status(top, PIC_H - PIC_BORDER, SPRT_BYTES, "pic"));
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_BOUNDS, open_status(top, UINT8_MAX, SPRT_BYTES, "pic"));
}

static void
test_a_one_bit_picture_absent_name_or_other_type_is_refused(void) {
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_FORMAT, open_status(-1, 0, SPRT_BYTES, "mono"));
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_NOT_FOUND, open_status(-1, 0, SPRT_BYTES, "absent"));
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_TYPE, open_status(-1, 0, SPRT_BYTES, "other"));
}

/* The test tile, opened from its pack; NULL pack when the pack is missing. */
static const asset_pack_t*
open_tile(handle_t* load, sprite_t* tile) {
    *load = asset_store_load(SPRITE_TEST_PACK);
    const asset_pack_t* pack = asset_store_loaded(*load);
    TEST_ASSERT_NOT_NULL_MESSAGE(pack, "no sprite_test pack: see the log above");
    TEST_ASSERT_EQUAL_INT(ASSET_OK, draw_sprite_open(pack, SPRITE_TEST_TILE, tile));
    return pack;
}

static void
test_the_test_tile_opens_translucent_with_a_flat_centre(void) {
    handle_t load;
    sprite_t tile;
    (void)open_tile(&load, &tile);
    asset_store_release(load);
    TEST_ASSERT_EQUAL_UINT8(TILE_BORDER, tile.border.left);
    TEST_ASSERT_EQUAL_UINT8(TILE_BORDER, tile.border.bottom);
    TEST_ASSERT_NOT_NULL(tile.alpha.pixels);
    TEST_ASSERT_TRUE(tile.flat_centre);
    TEST_ASSERT_EQUAL_UINT8(TILE_CENTRE, tile.centre_alpha);
}

#ifndef DEVICE_BUILD

#define BACKGROUND RGB565BE(0x203040)
#define FLAT_COLOR RGB565BE(0xC08040)

/* A 7 x 6 picture, every pixel its own colour, with a border of a different
 * width on each side so a turn or mirror shows. */
enum {
    GEO_W = 7,
    GEO_H = 6,
    GEO_LEFT = 1,
    GEO_TOP = 2,
    GEO_RIGHT = 2,
    GEO_BOTTOM = 1,
    GEO_AT = 20, /* where the blit lands on the panel */
    TURNS = 4,
    TINY = 2, /* a side shorter than its border pair */
    STRETCH = 3,
    CLIP_INSET = 3,
};

static gfx_color_t geo_pixels[GEO_H][GEO_W];

static gfx_color_t
code(int x, int y) {
    return (gfx_color_t)(0x100 * (y + 1) + x + 1);
}

static sprite_t
geometry_sprite(void) {
    for (int y = 0; y < GEO_H; y++) {
        for (int x = 0; x < GEO_W; x++) {
            geo_pixels[y][x] = code(x, y);
        }
    }
    return (sprite_t){
        .color = {geo_pixels, IMAGE_RGB565BE, GEO_W, GEO_H, GEO_W * sizeof(gfx_color_t)},
        .border = {GEO_LEFT, GEO_TOP, GEO_RIGHT, GEO_BOTTOM},
    };
}

static void
fixture(void) {
    TEST_ASSERT_TRUE(gfx_init());
    suite_set_test_cleanup(gfx_reset_for_test);
    draw_clear_clip();
    draw_clear(BACKGROUND);
}

static gfx_color_t
panel(int x, int y) {
    gfx_surface_t surface;
    TEST_ASSERT_TRUE(gfx_draw_begin(&surface));
    return gfx_target_color(surface.target, y)[x];
}

/* The nine-slice rule along one axis, from the design: output `at` of `out`
 * pixels shows: corners 1:1 from their own edge, a border pair longer than
 * the output shrunk in proportion, the centre by nearest pixel centre. */
static int
expected_source(int at, int out, int src, int lo, int hi) {
    int out_lo = lo;
    int out_hi = hi;
    if (lo + hi > out) {
        out_lo = lo * out / (lo + hi);
        out_hi = out - out_lo;
    }
    if (at < out_lo) {
        return at;
    }
    if (out - at <= out_hi) {
        return src - (out - at);
    }
    const double centre = (double)(src - lo - hi) / (out - out_lo - out_hi);
    return lo + (int)((at - out_lo + 0.5) * centre);
}

/* Every panel pixel of `dst` against the turned, nine-sliced picture: the
 * sprite frame pixel a panel pixel shows, by the turn image_asset.turn() makes. */
static void
assert_geometry(int turns, int w, int h) {
    const sprite_t s = geometry_sprite();
    const bool across = (turns & 1) != 0;
    const recti_t dst = {GEO_AT, GEO_AT, across ? h : w, across ? w : h};
    draw_clear(BACKGROUND);
    draw_sprite(&s, dst, turns);
    for (int py = 0; py < dst.h; py++) {
        for (int px = 0; px < dst.w; px++) {
            const int lx[TURNS] = {px, py, w - 1 - px, w - 1 - py};
            const int ly[TURNS] = {py, h - 1 - px, h - 1 - py, px};
            const int sx = expected_source(lx[turns], w, GEO_W, GEO_LEFT, GEO_RIGHT);
            const int sy = expected_source(ly[turns], h, GEO_H, GEO_TOP, GEO_BOTTOM);
            if (panel(dst.x + px, dst.y + py) != code(sx, sy)) {
                char message[96];
                (void)snprintf(message, sizeof message, "turn %d size %dx%d panel (%d, %d) is not source (%d, %d)",
                               turns, w, h, px, py, sx, sy);
                TEST_FAIL_MESSAGE(message);
            }
        }
    }
    TEST_ASSERT_EQUAL_HEX16(BACKGROUND, panel(dst.x - 1, dst.y));
    TEST_ASSERT_EQUAL_HEX16(BACKGROUND, panel(recti_right(dst), recti_bottom(dst) - 1));
}

static void
test_nine_slice_geometry_at_every_turn_and_size(void) {
    fixture();
    const int sizes[][2] = {
        {GEO_W, GEO_H},                        /* 1:1 */
        {GEO_W * STRETCH, GEO_H * STRETCH},    /* stretched both ways */
        {GEO_W + STRETCH + 1, GEO_H},          /* one axis only, odd */
        {GEO_LEFT + GEO_RIGHT + 1, GEO_H + 1}, /* one centre column */
        {TINY, TINY},                          /* shorter than the borders */
    };
    for (int turns = 0; turns < TURNS; turns++) {
        for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
            assert_geometry(turns, sizes[i][0], sizes[i][1]);
        }
    }
}

static void
test_a_stretched_corner_stays_1_to_1_and_an_edge_stretches_one_way(void) {
    fixture();
    const sprite_t s = geometry_sprite();
    const recti_t dst = {GEO_AT, GEO_AT, GEO_W * STRETCH, GEO_H * STRETCH};
    draw_sprite(&s, dst, 0);
    TEST_ASSERT_EQUAL_HEX16(code(0, 0), panel(dst.x, dst.y));
    TEST_ASSERT_EQUAL_HEX16(code(GEO_W - 1, GEO_TOP - 1), panel(recti_right(dst) - 1, dst.y + GEO_TOP - 1));
    TEST_ASSERT_EQUAL_HEX16(code(GEO_W - 1, GEO_H - 1), panel(recti_right(dst) - 1, recti_bottom(dst) - 1));
    for (int y = dst.y + GEO_TOP; y < recti_bottom(dst) - GEO_BOTTOM; y++) {
        TEST_ASSERT_EQUAL_INT(code(0, 0) & UINT8_MAX, panel(dst.x, y) & UINT8_MAX); /* the left edge: column 0 */
    }
}

static void
test_turned_the_top_border_lands_on_the_right(void) {
    fixture();
    const sprite_t s = geometry_sprite();
    const recti_t dst = {GEO_AT, GEO_AT, GEO_H, GEO_W};
    draw_sprite(&s, dst, 1);
    TEST_ASSERT_EQUAL_HEX16(code(0, 0), panel(recti_right(dst) - 1, dst.y));
    TEST_ASSERT_EQUAL_HEX16(code(0, GEO_H - 1), panel(dst.x, dst.y));
}

static void
test_coverage_0_leaves_255_replaces_and_between_mixes(void) {
    fixture();

    enum { COVER_W = 3, COVER_MID = 128 };

    static gfx_color_t color[COVER_W] = {0xF800, 0x07E0, 0x001F};
    static uint8_t alpha[COVER_W] = {0, UINT8_MAX, COVER_MID};
    const sprite_t s = {
        .color = {color, IMAGE_RGB565BE, COVER_W, 1, sizeof color},
        .alpha = {alpha, IMAGE_A8, COVER_W, 1, sizeof alpha},
    };
    draw_sprite(&s, (recti_t){GEO_AT, GEO_AT, COVER_W, 1}, 0);
    TEST_ASSERT_EQUAL_HEX16(BACKGROUND, panel(GEO_AT, GEO_AT));
    TEST_ASSERT_EQUAL_HEX16(color[1], panel(GEO_AT + 1, GEO_AT));
    TEST_ASSERT_EQUAL_HEX16(rgb565be_mix(BACKGROUND, color[2], COVER_MID), panel(GEO_AT + 2, GEO_AT));
}

static void
test_a_flat_centre_blends_its_one_colour_and_the_border_stays_per_pixel(void) {
    fixture();

    enum { FLAT_ALPHA = 96 };

    sprite_t s = geometry_sprite();
    s.flat_centre = true;
    s.centre_color = FLAT_COLOR;
    s.centre_alpha = FLAT_ALPHA;
    const recti_t dst = {GEO_AT, GEO_AT, GEO_W * STRETCH, GEO_H * STRETCH};
    draw_sprite(&s, dst, 0);
    const gfx_color_t centre = rgb565be_mix(BACKGROUND, FLAT_COLOR, FLAT_ALPHA);
    TEST_ASSERT_EQUAL_HEX16(centre, panel(dst.x + GEO_LEFT, dst.y + GEO_TOP));
    TEST_ASSERT_EQUAL_HEX16(centre, panel(recti_right(dst) - GEO_RIGHT - 1, recti_bottom(dst) - GEO_BOTTOM - 1));
    TEST_ASSERT_EQUAL_HEX16(code(0, GEO_TOP), panel(dst.x, dst.y + GEO_TOP));
    TEST_ASSERT_EQUAL_HEX16(code(GEO_LEFT, 0), panel(dst.x + GEO_LEFT, dst.y));
}

/* Drawn under a clip, every pixel inside it matches the unclipped draw and
 * every pixel outside keeps the background, flat centre or not. */
static void
assert_clipped(bool flat, int turns) {
    sprite_t s = geometry_sprite();
    s.flat_centre = flat;
    s.centre_color = FLAT_COLOR;
    s.centre_alpha = UINT8_MAX / 2;
    const recti_t dst = {GEO_AT, GEO_AT, GEO_W * STRETCH, GEO_H * STRETCH};
    static gfx_color_t whole[GEO_H * STRETCH][GEO_W * STRETCH];
    draw_clear_clip();
    draw_clear(BACKGROUND);
    draw_sprite(&s, dst, turns);
    for (int y = 0; y < dst.h; y++) {
        for (int x = 0; x < dst.w; x++) {
            whole[y][x] = panel(dst.x + x, dst.y + y);
        }
    }
    const recti_t clip = {dst.x + CLIP_INSET, dst.y + CLIP_INSET, dst.w - (2 * CLIP_INSET) - 1, CLIP_INSET * 2};
    draw_clear(BACKGROUND);
    draw_set_clip(clip.x, clip.y, clip.w, clip.h);
    draw_sprite(&s, dst, turns);
    draw_clear_clip();
    for (int y = 0; y < dst.h; y++) {
        for (int x = 0; x < dst.w; x++) {
            const bool inside = recti_contains(clip, dst.x + x, dst.y + y);
            TEST_ASSERT_EQUAL_HEX16(inside ? whole[y][x] : BACKGROUND, panel(dst.x + x, dst.y + y));
        }
    }
}

static void
test_a_clip_limits_every_pixel_of_the_blit(void) {
    fixture();
    for (int turns = 0; turns < TURNS; turns += 2) {
        assert_clipped(false, turns);
        assert_clipped(true, turns);
    }
}

static void
test_the_tile_draws_the_same_from_ram_as_from_its_pack(void) {
    fixture();

    enum { TILE_DST_W = 120, TILE_DST_H = 90 };

    handle_t load;
    sprite_t tile;
    (void)open_tile(&load, &tile);
    const recti_t dst = {GEO_AT, GEO_AT, TILE_DST_W, TILE_DST_H};
    static gfx_color_t mapped[TILE_DST_H][TILE_DST_W];
    draw_sprite(&tile, dst, 1);
    for (int y = 0; y < dst.h; y++) {
        for (int x = 0; x < dst.w; x++) {
            mapped[y][x] = panel(dst.x + x, dst.y + y);
        }
    }
    void* ram = malloc(draw_sprite_bytes(&tile));
    TEST_ASSERT_NOT_NULL(ram);
    const void* from_pack = tile.color.pixels;
    draw_sprite_make_resident(&tile, ram);
    asset_store_release(load);
    TEST_ASSERT_TRUE(tile.color.pixels != from_pack);
    draw_clear(BACKGROUND);
    draw_sprite(&tile, dst, 1);
    bool same = true;
    for (int y = 0; y < dst.h; y++) {
        for (int x = 0; x < dst.w; x++) {
            same = same && panel(dst.x + x, dst.y + y) == mapped[y][x];
        }
    }
    free(ram);
    TEST_ASSERT_TRUE_MESSAGE(same, "the resident tile drew differently");
    TEST_ASSERT_EQUAL_HEX16(BACKGROUND, mapped[0][0]); /* a clear rounded corner */
}

#endif

void
suite_draw_sprite(void) {
    RUN_TEST(test_a_sprite_opens_its_border_and_both_planes_in_place);
    RUN_TEST(test_a_bare_image_opens_as_a_sprite_with_no_border);
    RUN_TEST(test_each_bad_sprite_header_value_is_refused);
    RUN_TEST(test_a_border_leaving_no_centre_column_or_row_is_out_of_bounds);
    RUN_TEST(test_a_one_bit_picture_absent_name_or_other_type_is_refused);
    RUN_TEST(test_the_test_tile_opens_translucent_with_a_flat_centre);
#ifndef DEVICE_BUILD
    RUN_TEST(test_nine_slice_geometry_at_every_turn_and_size);
    RUN_TEST(test_a_stretched_corner_stays_1_to_1_and_an_edge_stretches_one_way);
    RUN_TEST(test_turned_the_top_border_lands_on_the_right);
    RUN_TEST(test_coverage_0_leaves_255_replaces_and_between_mixes);
    RUN_TEST(test_a_flat_centre_blends_its_one_colour_and_the_border_stays_per_pixel);
    RUN_TEST(test_a_clip_limits_every_pixel_of_the_blit);
    RUN_TEST(test_the_tile_draws_the_same_from_ram_as_from_its_pack);
#endif
}

SUITE_REGISTER(suite_draw_sprite);
SUITE_READS(suite_draw_sprite, SPRITE_TEST_PACK);
