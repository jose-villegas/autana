/*
 * Portable suite: options_screen - its geometry at both orientations with
 * no scrolling, where the DITHER list opens, every fixed string against its
 * own rect, the slider's direction, and real taps through microui.
 */

#include <stdio.h>
#include <string.h>

#include "suites.h"
#include "unity.h"

#include "gfx/gfx.h"
#include "gfx/gfx_font_roles.h"
#include "input/input.h"
#include "ui/ui.h"
#include "ui/ui_transform.h"
#include "ui/ui_widgets.h"

#include "options_screen.h"
#include "sand_theme.h"

#define TAP_TARGET_MIN 44

static const char* const QUALITY_NAMES[] = {"ULTRA", "HIGH", "NORMAL", "LOW", "VERY LOW"};
#define QUALITY_COUNT ((int)(sizeof QUALITY_NAMES / sizeof QUALITY_NAMES[0]))

static const char* const DITHER_NAMES[] = {"NONE", "CELL CHECKER", "CELL BAYER2", "PIXEL CHECKER2", "PIXEL BAYER4"};
#define DITHER_COUNT ((int)(sizeof DITHER_NAMES / sizeof DITHER_NAMES[0]))

/* Shaped like the app's, for layout, taps and command-list size; the
 * colours themselves are suite_sand_mode_swatches.c's. */
static const sand_mode_swatch_t MODE_SWATCHES[3] = {
    [SAND_COLOUR_FULL] = {.cols = SAND_SWATCH_FULL_BANDS, .rows = 1},
    [SAND_COLOUR_256] = {.cols = SAND_SWATCH_256_COLS, .rows = SAND_SWATCH_ROWS},
    [SAND_COLOUR_16] = {.cols = SAND_SWATCH_16_COLS, .rows = SAND_SWATCH_ROWS},
};

static const options_screen_labels_t LABELS = {
    .quality_names = QUALITY_NAMES,
    .quality_count = QUALITY_COUNT,
    .dither_names = DITHER_NAMES,
    .dither_count = DITHER_COUNT,
    .mode_swatches = MODE_SWATCHES,
};

static int
canvas_w(bool landscape) {
    return landscape ? GFX_HEIGHT : GFX_WIDTH;
}

static int
canvas_h(bool landscape) {
    return landscape ? GFX_WIDTH : GFX_HEIGHT;
}

static options_screen_layout_t
layout_for(bool landscape) {
    options_screen_layout_t lay;
    options_screen_layout(canvas_w(landscape), canvas_h(landscape), &lay);
    return lay;
}

static bool
inside(mu_Rect r, bool landscape) {
    return r.x >= 0 && r.y >= 0 && r.x + r.w <= canvas_w(landscape) && r.y + r.h <= canvas_h(landscape);
}

static bool
above(mu_Rect a, mu_Rect b) {
    return a.y + a.h <= b.y;
}

static void
assert_layout(bool landscape) {
    const options_screen_layout_t lay = layout_for(landscape);
    const mu_Rect stack[] = {lay.header,         lay.quality_panel, lay.color_caption, lay.tiles[0],
                             lay.dither_caption, lay.dither,        lay.apply};
    for (size_t i = 0; i < sizeof stack / sizeof stack[0]; i++) {
        TEST_ASSERT_TRUE_MESSAGE(inside(stack[i], landscape), "nothing may need scrolling to reach");
        if (i > 0) {
            TEST_ASSERT_TRUE_MESSAGE(above(stack[i - 1], stack[i]), "the screen's rows must stack without overlap");
        }
    }
    TEST_ASSERT_TRUE(inside(lay.cancel, landscape));

    const mu_Rect taps[] = {lay.quality_slider, lay.tiles[0], lay.tiles[1], lay.tiles[2],
                            lay.dither,         lay.apply,    lay.cancel};
    for (size_t i = 0; i < sizeof taps / sizeof taps[0]; i++) {
        TEST_ASSERT_GREATER_OR_EQUAL_INT(TAP_TARGET_MIN, taps[i].h);
    }
    TEST_ASSERT_TRUE(lay.apply.x + lay.apply.w <= lay.cancel.x);
    TEST_ASSERT_TRUE(lay.tiles[0].x + lay.tiles[0].w <= lay.tiles[1].x);
}

static void
test_the_whole_screen_fits_portrait(void) {
    assert_layout(false);
}

static void
test_the_whole_screen_fits_landscape(void) {
    assert_layout(true);
}

static void
assert_dither_list_opens_upward_on_screen(bool landscape) {
    const options_screen_layout_t lay = layout_for(landscape);
    const mu_Rect list = ui_dropdown_list_rect(lay.dither, DITHER_COUNT, lay.dither.h, canvas_h(landscape), UI_MARGIN);
    TEST_ASSERT_TRUE_MESSAGE(above(list, lay.dither), "the dropdown sits low, so its list grows upward");
    TEST_ASSERT_TRUE(inside(list, landscape));
}

static void
test_the_dither_list_opens_upward_portrait(void) {
    assert_dither_list_opens_upward_on_screen(false);
}

static void
test_the_dither_list_opens_upward_landscape(void) {
    assert_dither_list_opens_upward_on_screen(true);
}

static void
assert_fits(const char* str, int room) {
    TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE(room, gfx_font_text_width(gfx_font_ui(), str, -1, sand_ui_theme.text_scale),
                                          str);
}

static void
assert_strings_fit(bool landscape) {
    const options_screen_layout_t lay = layout_for(landscape);

    assert_fits(OPTIONS_SCREEN_TITLE, lay.header.w);
    assert_fits(OPTIONS_SCREEN_QUALITY, lay.quality_caption.w);
    for (int i = 0; i < QUALITY_COUNT; i++) {
        assert_fits(QUALITY_NAMES[i], lay.quality_value.w);
    }
    assert_fits(OPTIONS_SCREEN_COLOR_MODE, lay.color_caption.w);
    for (int i = 0; i < OPTIONS_SCREEN_TILE_COUNT; i++) {
        assert_fits(options_screen_tile_label(i), lay.tiles[i].w);
    }
    assert_fits(OPTIONS_SCREEN_DITHER, lay.dither_caption.w);
    for (int i = 0; i < DITHER_COUNT; i++) {
        assert_fits(DITHER_NAMES[i], ui_dropdown_label_width(lay.dither.w, &sand_ui_theme));
        assert_fits(DITHER_NAMES[i], ui_icon_button_label_width(lay.dither.w, true, &sand_ui_theme));
    }

    char apply[24];
    options_screen_apply_label(9, apply, (int)sizeof apply);
    assert_fits(apply, ui_icon_button_label_width(lay.apply.w, false, &sand_ui_theme));
    assert_fits(OPTIONS_SCREEN_CANCEL, ui_icon_button_label_width(lay.cancel.w, false, &sand_ui_theme));
}

static void
test_every_string_fits_its_rect_portrait(void) {
    assert_strings_fit(false);
}

static void
test_every_string_fits_its_rect_landscape(void) {
    assert_strings_fit(true);
}

static void
test_dragging_the_slider_right_means_finer_quality(void) {
    TEST_ASSERT_EQUAL_INT(0, options_screen_quality_from_slider(QUALITY_COUNT - 1, QUALITY_COUNT));
    TEST_ASSERT_EQUAL_INT(QUALITY_COUNT - 1, options_screen_quality_from_slider(0, QUALITY_COUNT));
    for (int q = 0; q < QUALITY_COUNT; q++) {
        const int slider = options_screen_slider_from_quality(q, QUALITY_COUNT);
        TEST_ASSERT_EQUAL_INT(q, options_screen_quality_from_slider(slider, QUALITY_COUNT));
    }
}

static void
test_apply_counts_what_is_pending(void) {
    char label[24];
    options_screen_apply_label(0, label, (int)sizeof label);
    TEST_ASSERT_EQUAL_STRING("APPLY", label);
    options_screen_apply_label(2, label, (int)sizeof label);
    TEST_ASSERT_EQUAL_STRING("APPLY (2)", label);
}

static void
test_tiles_run_sixteen_then_256_then_full(void) {
    TEST_ASSERT_EQUAL_INT(SAND_COLOUR_16, options_screen_tile_colour(0));
    TEST_ASSERT_EQUAL_INT(SAND_COLOUR_256, options_screen_tile_colour(1));
    TEST_ASSERT_EQUAL_INT(SAND_COLOUR_FULL, options_screen_tile_colour(2));
}

/* Real taps, portrait, through ui_begin()'s own pointer bridge. */

static sand_menu_t menu;

static sand_options_hits_t
options_frame(bool down, bool pressed, bool released, int x, int y) {
    input_t in = {.down = down, .pressed = pressed, .released = released, .x = x, .y = y};
    ui_begin(&in);
    const sand_options_hits_t hits = options_screen_draw(ui_context(), &menu, &LABELS);
    mu_end(ui_context());
    return hits;
}

static bool
any_hit(sand_options_hits_t h) {
    return h.quality >= 0 || h.color >= 0 || h.dither >= 0 || h.apply || h.cancel;
}

static sand_options_hits_t
tap_at(int x, int y) {
    sand_options_hits_t hit = SAND_OPTIONS_NO_HITS;
    sand_options_hits_t f = options_frame(true, true, false, x, y);
    hit = any_hit(f) ? f : hit;
    for (int i = 0; i < 4; i++) {
        f = options_frame(true, false, false, x, y);
        hit = any_hit(f) ? f : hit;
    }
    f = options_frame(false, false, true, x, y);
    hit = any_hit(f) ? f : hit;
    for (int i = 0; i < 2; i++) {
        f = options_frame(false, false, false, x, y);
        hit = any_hit(f) ? f : hit;
    }
    return hit;
}

static sand_options_hits_t
tap(mu_Rect r) {
    return tap_at(r.x + r.w / 2, r.y + r.h / 2);
}

static void
tap_fixture(sand_colour_mode_t colour) {
    ui_init();
    ui_set_transform(ui_transform_identity());
    sand_menu_init(&menu, (sand_options_t){.quality = 2, .color = colour, .dither = 2});
    sand_menu_title_clicked(&menu, SAND_TITLE_OPTIONS);
    options_frame(false, false, false, 0, 0);
    options_frame(false, false, false, 0, 0);
}

static void
test_a_tap_on_a_tile_reports_its_colour_mode(void) {
    tap_fixture(SAND_COLOUR_256);
    TEST_ASSERT_EQUAL_INT(SAND_COLOUR_FULL, tap(layout_for(false).tiles[2]).color);
}

static void
test_apply_takes_no_tap_while_nothing_is_pending(void) {
    tap_fixture(SAND_COLOUR_256);
    TEST_ASSERT_FALSE(tap(layout_for(false).apply).apply);
}

static void
test_apply_takes_a_tap_once_something_is_pending(void) {
    tap_fixture(SAND_COLOUR_256);
    menu.draft.quality = 0;
    TEST_ASSERT_TRUE(tap(layout_for(false).apply).apply);
}

static void
test_the_dither_dropdown_picks_from_its_list(void) {
    tap_fixture(SAND_COLOUR_16);
    const options_screen_layout_t lay = layout_for(false);
    TEST_ASSERT_FALSE(any_hit(tap(lay.dither)));

    const mu_Rect list = ui_dropdown_list_rect(lay.dither, DITHER_COUNT, lay.dither.h, canvas_h(false), UI_MARGIN);
    const sand_options_hits_t hits = tap_at(list.x + list.w / 2, list.y + lay.dither.h / 2);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, hits.dither, "the top row of the list is the first dither");
    TEST_ASSERT_FALSE_MESSAGE(hits.color >= 0, "the list, not a tile beneath it, takes the tap");
}

void
run_options_screen_suite(void) {
    RUN_TEST(test_the_whole_screen_fits_portrait);
    RUN_TEST(test_the_whole_screen_fits_landscape);
    RUN_TEST(test_the_dither_list_opens_upward_portrait);
    RUN_TEST(test_the_dither_list_opens_upward_landscape);
    RUN_TEST(test_every_string_fits_its_rect_portrait);
    RUN_TEST(test_every_string_fits_its_rect_landscape);
    RUN_TEST(test_dragging_the_slider_right_means_finer_quality);
    RUN_TEST(test_apply_counts_what_is_pending);
    RUN_TEST(test_tiles_run_sixteen_then_256_then_full);
    RUN_TEST(test_a_tap_on_a_tile_reports_its_colour_mode);
    RUN_TEST(test_apply_takes_no_tap_while_nothing_is_pending);
    RUN_TEST(test_apply_takes_a_tap_once_something_is_pending);
    RUN_TEST(test_the_dither_dropdown_picks_from_its_list);
}

SUITE_REGISTER(run_options_screen_suite);
