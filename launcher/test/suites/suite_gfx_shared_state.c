/*
 * Host suite: draw.c and gfx_present.c / gfx_mode.c share ONE instance of
 * the dirty tracker, framebuffer guard, present guard and band-force latch.
 * Draw writes through present's target and clip.
 *
 * A private copy per .c file would leave every header-level suite passing
 * while a draw never told the presenter anything. These tests go through the
 * real draw primitives and observe the effect through the present/mode side,
 * so a split copy fails here.
 */

#include "suites.h"
#include "unity.h"

#include "gfx_shared_state.h"

#include <stdlib.h>
#include <string.h>
#include "core/memory.h"
#include "draw/draw.h"
#include "draw/draw_font_roles.h"
#include "gfx/gfx.h"
#include "gfx/present/gfx_debug.h"
#include "gfx/present/gfx_mode.h"
#include "gfx/present/gfx_present.h"
#include "gfx/present/gfx_test.h"
#include "gfx/present/modes/band/gfx_band_run.h"
#include "testkit/cleanup.h"

#ifndef DEVICE_BUILD

enum { DRAWN_X = 10, DRAWN_Y = 10, OTHER_X = 200, OTHER_Y = 300, SIZE = 8 };

/* A presented, clean full-framebuffer frame to start every test from. */
static void
fixture(void) {
    if (gfx_mode_current()->width == 0) {
        TEST_ASSERT_TRUE(gfx_init());
    }
    suite_set_test_cleanup(gfx_reset_for_test);
    gfx_present();
    TEST_ASSERT_FALSE(gfx_region_dirty(0, 0, GFX_WIDTH, GFX_HEIGHT));
}

static void
assert_marked_only_where_drawn(void) {
    TEST_ASSERT_TRUE(gfx_region_dirty(DRAWN_X, DRAWN_Y, SIZE, SIZE));
    TEST_ASSERT_FALSE(gfx_region_dirty(OTHER_X, OTHER_Y, SIZE, SIZE));
    gfx_present();
    TEST_ASSERT_FALSE(gfx_region_dirty(0, 0, GFX_WIDTH, GFX_HEIGHT));
}

/* A public caller may pass rows past either screen edge; only on-screen
 * rows are marked. */
static void
test_row_marks_outside_the_screen_mark_nothing(void) {
    fixture();
    gfx_mark_rows_dirty(-SIZE, 0);
    gfx_mark_rows_dirty(GFX_HEIGHT, GFX_HEIGHT + SIZE);
    TEST_ASSERT_FALSE(gfx_region_dirty(0, 0, GFX_WIDTH, GFX_HEIGHT));
    gfx_mark_rows_dirty(-SIZE, 1);
    TEST_ASSERT_TRUE(gfx_region_dirty(0, 0, 1, 1));
}

static void
test_box_marking_draw_is_seen_by_the_presenter(void) {
    fixture();
    draw_rect(DRAWN_X, DRAWN_Y, SIZE, SIZE, 0x1234);
    assert_marked_only_where_drawn();
}

static void
test_row_marking_draw_is_seen_by_the_presenter(void) {
    fixture();
    draw_pixel(DRAWN_X, DRAWN_Y, 0x1234);
    assert_marked_only_where_drawn();
}

static const gfx_mode_request_t indexed_request = {
    .layout = GFX_LAYOUT_INDEXED,
    .index_grid_w = 1,
    .index_grid_h = 1,
    .cell_size = 1,
};

static void
test_draw_is_refused_while_a_mode_has_freed_the_framebuffer(void) {
    fixture();
    TEST_ASSERT_EQUAL_INT(GFX_LAYOUT_INDEXED, gfx_mode_enter(&indexed_request)->layout);
    const unsigned before = gfx_fb_guard_trips;
    draw_rect(DRAWN_X, DRAWN_Y, SIZE, SIZE, 0x1234);
    TEST_ASSERT_EQUAL_UINT(before + 1, gfx_fb_guard_trips);
    draw_pixel(DRAWN_X, DRAWN_Y, 0x1234);
    TEST_ASSERT_EQUAL_UINT(before + 2, gfx_fb_guard_trips);

    gfx_mode_exit();
    const unsigned after_exit = gfx_fb_guard_trips;
    draw_pixel(DRAWN_X, DRAWN_Y, 0x4321);
    TEST_ASSERT_EQUAL_UINT(after_exit, gfx_fb_guard_trips);
    TEST_ASSERT_EQUAL_HEX16(0x4321, gfx_framebuffer()[DRAWN_Y * GFX_WIDTH + DRAWN_X]);
}

enum { INDEX_GRID_W = 6, INDEX_GRID_H = 3 };

static const gfx_mode_request_t index_grid_request = {
    .layout = GFX_LAYOUT_INDEXED,
    .index_grid_w = INDEX_GRID_W,
    .index_grid_h = INDEX_GRID_H,
    .cell_size = 1,
};

static void
test_the_index_image_is_the_granted_grid(void) {
    fixture();
    TEST_ASSERT_EQUAL_INT(GFX_LAYOUT_INDEXED, gfx_mode_enter(&index_grid_request)->layout);

    const image_t index = gfx_indexed_image();
    TEST_ASSERT_NOT_NULL(index.pixels);
    TEST_ASSERT_EQUAL_INT(IMAGE_INDEXED8, index.format);
    TEST_ASSERT_EQUAL_INT(INDEX_GRID_W, index.width);
    TEST_ASSERT_EQUAL_INT(INDEX_GRID_H, index.height);
    TEST_ASSERT_EQUAL_size_t(INDEX_GRID_W, index.pitch);

    gfx_mode_exit();
}

/* Each index on row 0 reads back as that index's own palette entry. */
static void
test_the_installed_palette_is_what_indices_expand_through(void) {
    fixture();
    TEST_ASSERT_EQUAL_INT(GFX_LAYOUT_INDEXED, gfx_mode_enter(&index_grid_request)->layout);

    gfx_color_t* entries = malloc(GFX_INDEXED_PALETTE_SIZE * sizeof *entries);
    gfx_color_t* row = malloc(GFX_WIDTH * sizeof *row);
    TEST_ASSERT_NOT_NULL(entries);
    TEST_ASSERT_NOT_NULL(row);
    for (int i = 0; i < GFX_INDEXED_PALETTE_SIZE; i++) {
        entries[i] = (gfx_color_t)(0x0101U * (unsigned)i);
    }
    const palette_t palette = {"test", entries, GFX_INDEXED_PALETTE_SIZE};
    gfx_indexed_set_palette(&palette);

    const image_t index = gfx_indexed_image();
    uint8_t* cells = image_row(&index, 0);
    for (int x = 0; x < INDEX_GRID_W; x++) {
        cells[x] = (uint8_t)(GFX_INDEXED_PALETTE_SIZE - 1 - (x * 37));
    }
    TEST_ASSERT_EQUAL_INT(GFX_READBACK_READY, gfx_readback_begin());
    gfx_read_panel_row(0, row);
    gfx_readback_end();
    for (int x = 0; x < INDEX_GRID_W; x++) {
        TEST_ASSERT_EQUAL_HEX16(entries[cells[x]], row[x]);
    }

    free(row);
    free(entries);
    gfx_mode_exit();
}

/* Enters `request` while `kind` holds no free block of `block_bytes`: holds
 * such blocks until no more fit, each keeping the one before it, then gives
 * them all back. Returns the layout granted. */
static gfx_layout_t
enter_with_memory_held(const gfx_mode_request_t* request, size_t block_bytes, memory_kind_t kind) {
    void* held = NULL;
    void* block;
    while ((block = memory_alloc(block_bytes, kind)) != NULL) {
        *(void**)block = held;
        held = block;
    }
    const gfx_layout_t granted = gfx_mode_enter(request)->layout;
    while (held != NULL) {
        void* const next = *(void**)held;
        memory_free(held);
        held = next;
    }
    return granted;
}

/* Every gfx_mode_enter() that stays FULL for want of storage counts once, in
 * any layout; a grant counts nothing. */
static void
test_a_refused_layout_counts_once_and_a_granted_one_not_at_all(void) {
    fixture();
    const unsigned before = gfx_mode_denials();

    TEST_ASSERT_EQUAL_INT(GFX_LAYOUT_INDEXED, gfx_mode_enter(&indexed_request)->layout);
    gfx_mode_exit();
    TEST_ASSERT_EQUAL_UINT(before, gfx_mode_denials());

    const gfx_mode_request_t no_cells = {.layout = GFX_LAYOUT_INDEXED};
    TEST_ASSERT_EQUAL_INT_MESSAGE(GFX_LAYOUT_FULL_FB, gfx_mode_enter(&no_cells)->layout, "no cells");
    const gfx_mode_request_t past_the_pool = {
        .layout = GFX_LAYOUT_INDEXED,
        .index_grid_w = (int)memory_total_bytes(MEMORY_INTERNAL) + 1,
        .index_grid_h = 1,
        .cell_size = 1,
    };
    TEST_ASSERT_EQUAL_INT_MESSAGE(GFX_LAYOUT_FULL_FB, gfx_mode_enter(&past_the_pool)->layout, "past the pool");
    TEST_ASSERT_EQUAL_UINT(before + 2, gfx_mode_denials());

    const gfx_mode_request_t bands = {.layout = GFX_LAYOUT_BANDS};
    const size_t band_bytes = (size_t)GFX_WIDTH * GFX_BAND_HEIGHT * sizeof(gfx_color_t);
    TEST_ASSERT_EQUAL_INT_MESSAGE(GFX_LAYOUT_FULL_FB, enter_with_memory_held(&bands, band_bytes, MEMORY_INTERNAL),
                                  "bands with internal RAM held");
    TEST_ASSERT_EQUAL_UINT(before + 3, gfx_mode_denials());

    const gfx_mode_request_t double_fb = {.layout = GFX_LAYOUT_DOUBLE_FB};
    TEST_ASSERT_EQUAL_INT_MESSAGE(GFX_LAYOUT_FULL_FB,
                                  enter_with_memory_held(&double_fb, GFX_FRAMEBUFFER_BYTES, MEMORY_PSRAM),
                                  "double framebuffer with PSRAM held");
    TEST_ASSERT_EQUAL_UINT(before + 4, gfx_mode_denials());
}

static void
test_band_mode_refuses_draws_too(void) {
    fixture();
    const gfx_mode_request_t request = {.layout = GFX_LAYOUT_BANDS};
    TEST_ASSERT_EQUAL_INT(GFX_LAYOUT_BANDS, gfx_mode_enter(&request)->layout);
    const unsigned before = gfx_fb_guard_trips;
    draw_rect(DRAWN_X, DRAWN_Y, SIZE, SIZE, 0x1234);
    TEST_ASSERT_EQUAL_UINT(before + 1, gfx_fb_guard_trips);
    gfx_mode_exit();
}

static void
test_draw_between_present_begin_and_wait_trips_the_present_guard(void) {
    fixture();
    gfx_present_begin();
    unsigned before = gfx_present_guard_trips;
    draw_rect(DRAWN_X, DRAWN_Y, SIZE, SIZE, 0x1234);
    TEST_ASSERT_GREATER_THAN_UINT(before, gfx_present_guard_trips);
    before = gfx_present_guard_trips;
    draw_pixel(DRAWN_X, DRAWN_Y, 0x1234);
    TEST_ASSERT_GREATER_THAN_UINT(before, gfx_present_guard_trips);
    gfx_present_wait();

    before = gfx_present_guard_trips;
    draw_rect(DRAWN_X, DRAWN_Y, SIZE, SIZE, 0x1234);
    TEST_ASSERT_EQUAL_UINT(before, gfx_present_guard_trips);
}

enum { INK = UINT16_MAX, BAND_ROW = GFX_BAND_HEIGHT, ALPHA_OPAQUE = UINT8_MAX };

enum { DRAW_CLEAR, DRAW_PIXEL, DRAW_LINE, DRAW_FILL, DRAW_DITHER, DRAW_BLEND, DRAW_BLIT, DRAW_TEXT, DRAW_TEXT_DITHER };

static void
primitive(int kind, int y) {
    const draw_font_t* font = draw_font_ui();
    gfx_color_t source[SIZE * SIZE];
    for (size_t i = 0; i < sizeof source / sizeof *source; i++) {
        source[i] = INK;
    }
    const image_t image = {
        .pixels = source, .format = IMAGE_RGB565BE, .width = SIZE, .height = SIZE, .pitch = SIZE * sizeof(gfx_color_t)};
    switch (kind) {
        case DRAW_CLEAR: draw_clear(INK); break;
        case DRAW_PIXEL: draw_pixel(DRAWN_X, y, INK); break;
        case DRAW_LINE: draw_line(DRAWN_X, y, DRAWN_X + SIZE, y + SIZE, INK); break;
        case DRAW_FILL: draw_rect(DRAWN_X, y, SIZE, SIZE, INK); break;
        case DRAW_DITHER: draw_rect_dither(DRAWN_X, y, SIZE, SIZE, INK, ALPHA_OPAQUE); break;
        case DRAW_BLEND: draw_rect_blend(DRAWN_X, y, SIZE, SIZE, INK, ALPHA_OPAQUE); break;
        case DRAW_BLIT: draw_blit_dither(DRAWN_X, y, &image, ALPHA_OPAQUE); break;
        case DRAW_TEXT: draw_text_font(DRAWN_X, y, "H", INK, 1, 0, font); break;
        case DRAW_TEXT_DITHER: draw_text_font_dither(DRAWN_X, y, "H", INK, 1, 0, font, ALPHA_OPAQUE); break;
    }
}

static void
check_band_marks(int kind) {
    fixture();
    gfx_band_begin_for_test(BAND_ROW);
    primitive(kind, BAND_ROW);
    gfx_band_end_for_test();
    TEST_ASSERT_FALSE(gfx_region_dirty(0, 0, GFX_WIDTH, GFX_HEIGHT));
    primitive(kind, BAND_ROW);
    TEST_ASSERT_TRUE(gfx_region_dirty(DRAWN_X, BAND_ROW, SIZE, SIZE));
}

static void
test_band_target_writes_only_local_rows(void) {
    fixture();
    draw_clear(0);
    gfx_present();
    gfx_band_begin_for_test(BAND_ROW);
    draw_clear(INK);
    draw_rect(0, 0, GFX_WIDTH, GFX_HEIGHT, INK);
    gfx_band_end_for_test();
    const gfx_color_t* pixels = gfx_framebuffer();
    for (int y = 0; y < GFX_HEIGHT; y++) {
        for (int x = 0; x < GFX_WIDTH; x++) {
            const gfx_color_t expected = y >= BAND_ROW && y < BAND_ROW + GFX_BAND_HEIGHT ? INK : 0;
            TEST_ASSERT_EQUAL_HEX16(expected, pixels[y * GFX_WIDTH + x]);
        }
    }
}

static void
test_clip_edges_match_independent_clamps(void) {
    fixture();
    static const int xs[] = {-SIZE, 0, SIZE, GFX_WIDTH, GFX_WIDTH + SIZE};
    static const int ys[] = {-SIZE, 0, SIZE, GFX_HEIGHT, GFX_HEIGHT + SIZE};
    static const int widths[] = {-SIZE, 0, SIZE, GFX_WIDTH + SIZE};
    static const int heights[] = {-SIZE, 0, SIZE, GFX_HEIGHT + SIZE};
    for (size_t i = 0; i < sizeof xs / sizeof *xs; i++) {
        for (size_t j = 0; j < sizeof ys / sizeof *ys; j++) {
            for (size_t k = 0; k < sizeof widths / sizeof *widths; k++) {
                for (size_t l = 0; l < sizeof heights / sizeof *heights; l++) {
                    draw_set_clip(xs[i], ys[j], widths[k], heights[l]);
                    gfx_surface_t surface;
                    TEST_ASSERT_TRUE(gfx_draw_begin(&surface));
                    const recti_t actual = surface.clip;
                    const recti_t expected = recti_from_edges(mathi_max(xs[i], 0), mathi_max(ys[j], 0),
                                                              mathi_min(xs[i] + widths[k], GFX_WIDTH),
                                                              mathi_min(ys[j] + heights[l], GFX_HEIGHT));
                    TEST_ASSERT_EQUAL_MEMORY(&expected, &actual, sizeof actual);
                    draw_clear(0);
                    draw_rect(-SIZE, -SIZE, GFX_WIDTH + SIZE * 2, GFX_HEIGHT + SIZE * 2, INK);
                    const gfx_color_t* pixels = gfx_framebuffer();
                    for (int y = 0; y < GFX_HEIGHT; y++) {
                        bool row_matches = true;
                        for (int x = 0; x < GFX_WIDTH; x++) {
                            const bool inside = x >= expected.x && x < recti_right(expected) && y >= expected.y
                                                && y < recti_bottom(expected);
                            row_matches &= pixels[y * GFX_WIDTH + x] == (inside ? INK : 0);
                        }
                        TEST_ASSERT_TRUE(row_matches);
                    }
                }
            }
        }
    }
}

static gfx_color_t* text_pixels;

static void
release_text_pixels(void) {
    free(text_pixels);
    text_pixels = NULL;
    gfx_reset_for_test();
}

static void
draw_text_parameters(bool dither, int scale, int turn) {
    const int x = SIZE * 4;
    const int y = BAND_ROW + SIZE * 4;
    if (dither) {
        draw_text_font_dither(x, y, "Fj", INK, scale, turn, draw_font_ui(), ALPHA_OPAQUE / 2);
    } else {
        draw_text_font(x, y, "Fj", INK, scale, turn, draw_font_ui());
    }
}

static void
check_text_parameters(bool turns) {
    fixture();

    enum { TEXT_ROWS = SIZE * 8 };

    static const int rotations[][2] = {{-9, 3}, {-5, 3}, {-4, 0}, {-3, 1}, {-1, 3}, {4, 0}, {5, 1}, {7, 3}, {9, 1}};
    static const int scales[][2] = {{0, 1}, {-1, 1}, {-SIZE, 1}};
    const int (*cases)[2] = turns ? rotations : scales;
    const size_t count = turns ? sizeof rotations / sizeof *rotations : sizeof scales / sizeof *scales;
    const size_t row_bytes = GFX_WIDTH * sizeof *text_pixels;
    text_pixels = malloc(TEXT_ROWS * row_bytes);
    suite_set_test_cleanup(release_text_pixels);
    TEST_ASSERT_NOT_NULL(text_pixels);
    for (int dither = 0; dither <= 1; dither++) {
        for (size_t i = 0; i < count; i++) {
            draw_clear(0);
            draw_text_parameters(dither, turns ? 1 : cases[i][1], turns ? cases[i][1] : 0);
            memcpy(text_pixels, gfx_framebuffer() + BAND_ROW * GFX_WIDTH, TEXT_ROWS * row_bytes);
            int ink = 0;
            for (int p = 0; p < TEXT_ROWS * GFX_WIDTH; p++) {
                ink += text_pixels[p] != 0;
            }
            TEST_ASSERT_GREATER_THAN_INT(0, ink);
            draw_clear(0);
            draw_text_parameters(dither, turns ? 1 : cases[i][0], turns ? cases[i][0] : 0);
            for (int row = 0; row < TEXT_ROWS; row++) {
                TEST_ASSERT_EQUAL_MEMORY(text_pixels + row * GFX_WIDTH,
                                         gfx_framebuffer() + (BAND_ROW + row) * GFX_WIDTH, row_bytes);
            }
        }
    }
}

static void
test_text_turns_wrap_to_equivalent_quarter_turn(void) {
    check_text_parameters(true);
}

static void
test_text_scale_below_one_matches_one(void) {
    check_text_parameters(false);
}

static void
test_clear_clip_restores_full_screen_draw(void) {
    fixture();
    draw_clear(0);
    draw_set_clip(DRAWN_X, DRAWN_Y, SIZE, SIZE);
    draw_clear_clip();
    draw_rect(0, 0, GFX_WIDTH, GFX_HEIGHT, INK);
    for (int y = 0; y < GFX_HEIGHT; y++) {
        bool row_matches = true;
        for (int x = 0; x < GFX_WIDTH; x++) {
            row_matches &= gfx_framebuffer()[y * GFX_WIDTH + x] == INK;
        }
        TEST_ASSERT_TRUE(row_matches);
    }
}

static void
test_primitives_crossing_band_edges_stay_in_strip(void) {
    fixture();
    static const int kinds[] = {DRAW_BLIT, DRAW_PIXEL, DRAW_LINE, DRAW_DITHER, DRAW_BLEND, DRAW_TEXT, DRAW_TEXT_DITHER};
    static const int origins[] = {BAND_ROW - SIZE / 2, BAND_ROW, BAND_ROW + GFX_BAND_HEIGHT - SIZE / 2,
                                  BAND_ROW + GFX_BAND_HEIGHT};
    const size_t row_bytes = GFX_WIDTH * sizeof *text_pixels;
    text_pixels = malloc(GFX_BAND_HEIGHT * row_bytes);
    suite_set_test_cleanup(release_text_pixels);
    TEST_ASSERT_NOT_NULL(text_pixels);
    for (size_t k = 0; k < sizeof kinds / sizeof *kinds; k++) {
        int ink = 0;
        for (size_t i = 0; i < sizeof origins / sizeof *origins; i++) {
            draw_clear(0);
            primitive(kinds[k], origins[i]);
            memcpy(text_pixels, gfx_framebuffer() + BAND_ROW * GFX_WIDTH, GFX_BAND_HEIGHT * row_bytes);
            draw_clear(0);
            gfx_present();
            gfx_band_begin_for_test(BAND_ROW);
            primitive(kinds[k], origins[i]);
            gfx_band_end_for_test();
            for (int y = 0; y < GFX_HEIGHT; y++) {
                if (y >= BAND_ROW && y < BAND_ROW + GFX_BAND_HEIGHT) {
                    TEST_ASSERT_EQUAL_MEMORY(text_pixels + (y - BAND_ROW) * GFX_WIDTH,
                                             gfx_framebuffer() + y * GFX_WIDTH, row_bytes);
                    for (int x = 0; x < GFX_WIDTH; x++) {
                        ink += gfx_framebuffer()[y * GFX_WIDTH + x] != 0;
                    }
                } else {
                    bool clean = true;
                    for (int x = 0; x < GFX_WIDTH; x++) {
                        clean &= gfx_framebuffer()[y * GFX_WIDTH + x] == 0;
                    }
                    TEST_ASSERT_TRUE(clean);
                }
            }
            TEST_ASSERT_FALSE(gfx_region_dirty(0, 0, GFX_WIDTH, GFX_HEIGHT));
        }
        TEST_ASSERT_GREATER_THAN_INT(0, ink);
    }
}

static void
check_text_clip(int kind) {
    fixture();
    const draw_font_t* font = draw_font_ui();
    const size_t bytes = sizeof *text_pixels * font->cell_w * font->cell_h;
    text_pixels = malloc(bytes);
    suite_set_test_cleanup(release_text_pixels);
    TEST_ASSERT_NOT_NULL(text_pixels);
    draw_clear(0);
    primitive(kind, BAND_ROW);
    for (int row = 0; row < font->cell_h; row++) {
        memcpy(text_pixels + row * font->cell_w, gfx_framebuffer() + (BAND_ROW + row) * GFX_WIDTH + DRAWN_X,
               sizeof *text_pixels * font->cell_w);
    }
    const int left = DRAWN_X + font->cell_w / 2;
    const int top = BAND_ROW + font->cell_h / 2;
    draw_clear(0);
    gfx_present();
    draw_set_clip(left, top, font->cell_w, font->cell_h);
    primitive(kind, BAND_ROW);
    int ink = 0;
    for (int y = 0; y < GFX_HEIGHT; y++) {
        for (int x = 0; x < GFX_WIDTH; x++) {
            const bool inside = x >= left && x < left + font->cell_w && y >= top && y < top + font->cell_h;
            const gfx_color_t expected = inside && x < DRAWN_X + font->cell_w && y < BAND_ROW + font->cell_h
                                             ? text_pixels[(y - BAND_ROW) * font->cell_w + x - DRAWN_X]
                                             : 0;
            TEST_ASSERT_EQUAL_HEX16(expected, gfx_framebuffer()[y * GFX_WIDTH + x]);
            if (expected != 0) {
                ink++;
                TEST_ASSERT_TRUE(gfx_region_dirty(x, y, 1, 1));
            }
        }
    }
    TEST_ASSERT_GREATER_THAN_INT(0, ink);
    TEST_ASSERT_FALSE(gfx_region_dirty(OTHER_X, OTHER_Y, SIZE, SIZE));
}

#define BAND_MARK_TEST(name, kind)                                                                                     \
    static void test_band_marks_##name(void) { check_band_marks(kind); }

BAND_MARK_TEST(clear, DRAW_CLEAR)
BAND_MARK_TEST(pixel, DRAW_PIXEL)
BAND_MARK_TEST(line, DRAW_LINE)
BAND_MARK_TEST(fill, DRAW_FILL)
BAND_MARK_TEST(dither, DRAW_DITHER)
BAND_MARK_TEST(blend, DRAW_BLEND)
BAND_MARK_TEST(blit, DRAW_BLIT)
BAND_MARK_TEST(text, DRAW_TEXT)
BAND_MARK_TEST(text_dither, DRAW_TEXT_DITHER)

#undef BAND_MARK_TEST

static void
test_clip_text(void) {
    check_text_clip(DRAW_TEXT);
}

static void
test_clip_text_dither(void) {
    check_text_clip(DRAW_TEXT_DITHER);
}

enum { BAND_MAX = GFX_HEIGHT / 16 };

static int band_draws;

static void
count_draw(int row0, int row1, gfx_color_t* target) {
    (void)row0;
    (void)row1;
    (void)target;
    band_draws++;
}

static int
run_bands(void) {
    band_draws = 0;
    gfx_band_run(count_draw, NULL);
    return band_draws;
}

/* gfx_invalidate() sets the force latch gfx_mode.c's band loop reads. The
 * Bands run until a frame sends none first, since enter forces its own full
 * frame. */
static void
test_invalidate_alone_forces_one_full_band_frame(void) {
    fixture();
    const gfx_mode_request_t request = {.layout = GFX_LAYOUT_BANDS};
    TEST_ASSERT_EQUAL_INT(GFX_LAYOUT_BANDS, gfx_mode_enter(&request)->layout);
    const int bands = gfx_mode_current()->height / gfx_mode_current()->band_height;
    TEST_ASSERT_TRUE(bands > 0 && bands <= BAND_MAX);

    for (int settle = 0; settle < 4 && run_bands() != 0; settle++) {}
    TEST_ASSERT_EQUAL_INT(0, run_bands());

    gfx_invalidate();
    TEST_ASSERT_EQUAL_INT(bands, run_bands());
    TEST_ASSERT_EQUAL_INT(0, run_bands());
    gfx_mode_exit();
}

#endif

void
run_gfx_shared_state_suite(void) {
    gfx_shared_state_t saved;
    gfx_shared_state_save(&saved);
#ifndef DEVICE_BUILD
    RUN_TEST(test_the_index_image_is_the_granted_grid);
    RUN_TEST(test_the_installed_palette_is_what_indices_expand_through);
    RUN_TEST(test_text_turns_wrap_to_equivalent_quarter_turn);
    RUN_TEST(test_text_scale_below_one_matches_one);
    RUN_TEST(test_clear_clip_restores_full_screen_draw);
    RUN_TEST(test_primitives_crossing_band_edges_stay_in_strip);
    RUN_TEST(test_band_target_writes_only_local_rows);
    RUN_TEST(test_clip_edges_match_independent_clamps);
    RUN_TEST(test_band_marks_clear);
    RUN_TEST(test_band_marks_pixel);
    RUN_TEST(test_band_marks_line);
    RUN_TEST(test_band_marks_fill);
    RUN_TEST(test_band_marks_dither);
    RUN_TEST(test_band_marks_blend);
    RUN_TEST(test_band_marks_blit);
    RUN_TEST(test_band_marks_text);
    RUN_TEST(test_band_marks_text_dither);
    RUN_TEST(test_clip_text);
    RUN_TEST(test_clip_text_dither);
    RUN_TEST(test_row_marks_outside_the_screen_mark_nothing);
    RUN_TEST(test_box_marking_draw_is_seen_by_the_presenter);
    RUN_TEST(test_row_marking_draw_is_seen_by_the_presenter);
    RUN_TEST(test_draw_is_refused_while_a_mode_has_freed_the_framebuffer);
    RUN_TEST(test_band_mode_refuses_draws_too);
    RUN_TEST(test_a_refused_layout_counts_once_and_a_granted_one_not_at_all);
    RUN_TEST(test_draw_between_present_begin_and_wait_trips_the_present_guard);
    RUN_TEST(test_invalidate_alone_forces_one_full_band_frame);
#endif
    gfx_shared_state_restore(&saved);
}

SUITE_REGISTER(run_gfx_shared_state_suite);
