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
#include "ui/ui_internal.h"
#include "ui/ui_transform.h"
#include "ui/ui_widgets.h"

#include "apps/sand/ui/options_screen.h"
#include "apps/sand/ui/sand_theme.h"

static const char* const QUALITY_NAMES[] = {"ULTRA", "HIGH", "NORMAL", "LOW", "VERY LOW"};
#define QUALITY_COUNT ((int)(sizeof QUALITY_NAMES / sizeof QUALITY_NAMES[0]))

static const char* const DITHER_NAMES[] = {"NONE", "CELL CHECKER", "CELL BAYER2", "PIXEL CHECKER2", "PIXEL BAYER4"};
#define DITHER_COUNT ((int)(sizeof DITHER_NAMES / sizeof DITHER_NAMES[0]))

/* Shaped like the app's, for layout, taps and command-list size; the
 * colours themselves are suite_sand_mode_swatches.c's. */
static const sand_mode_swatch_t MODE_SWATCHES[SAND_COLOUR_MODE_COUNT] = {
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
        TEST_ASSERT_GREATER_OR_EQUAL_INT(UI_TAP_MIN, taps[i].w < taps[i].h ? taps[i].w : taps[i].h);
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
static sand_options_t committed;

static sand_options_hits_t
options_frame(bool down, bool pressed, bool released, int x, int y) {
    input_t in = {.down = down, .pressed = pressed, .released = released, .x = x, .y = y};
    ui_begin(&in);
    const sand_options_hits_t hits = options_screen_draw(ui_context(), &menu, committed, &LABELS);
    mu_end(ui_context());
    ui_pointer_state.over_scrollable = ui_ctx.scroll_target != NULL;
    return hits;
}

static bool
any_hit(sand_options_hits_t h) {
    return h.quality >= 0 || h.color >= 0 || h.dither >= 0 || h.apply || h.cancel;
}

static sand_options_hits_t
tap_at(int x, int y) {
    int px, py;
    ui_transform_point(ui_transform_quarter_turn(ui_width() == GFX_HEIGHT ? 1 : 0, GFX_WIDTH, GFX_HEIGHT), x, y, &px,
                       &py);
    sand_options_hits_t hit = SAND_OPTIONS_NO_HITS;
    sand_options_hits_t f = options_frame(true, true, false, px, py);
    hit = any_hit(f) ? f : hit;
    for (int i = 0; i < 4; i++) {
        f = options_frame(true, false, false, px, py);
        hit = any_hit(f) ? f : hit;
    }
    f = options_frame(false, false, true, px, py);
    hit = any_hit(f) ? f : hit;
    for (int i = 0; i < 2; i++) {
        f = options_frame(false, false, false, px, py);
        hit = any_hit(f) ? f : hit;
    }
    return hit;
}

static sand_options_hits_t
tap(mu_Rect r) {
    return tap_at(r.x + r.w / 2, r.y + r.h / 2);
}

static void
tap_fixture(sand_colour_mode_t colour, bool landscape) {
    ui_init();
    ui_set_transform(ui_transform_quarter_turn(landscape ? 1 : 0, GFX_WIDTH, GFX_HEIGHT));
    committed = (sand_options_t){.quality = 2, .color = colour, .dither = 2};
    sand_menu_init(&menu);
    sand_menu_title_clicked(&menu, SAND_TITLE_OPTIONS, committed);
    options_frame(false, false, false, 0, 0);
    options_frame(false, false, false, 0, 0);
}

static void
test_a_tap_on_a_tile_reports_its_colour_mode(void) {
    tap_fixture(SAND_COLOUR_256, false);
    TEST_ASSERT_EQUAL_INT(SAND_COLOUR_FULL, tap(layout_for(false).tiles[2]).color);
}

static void
test_a_landscape_tap_on_a_tile_reports_its_colour_mode(void) {
    tap_fixture(SAND_COLOUR_256, true);
    TEST_ASSERT_EQUAL_INT(SAND_COLOUR_FULL, tap(layout_for(true).tiles[2]).color);
}

static void
test_apply_takes_no_tap_while_nothing_is_pending(void) {
    tap_fixture(SAND_COLOUR_256, false);
    TEST_ASSERT_FALSE(tap(layout_for(false).apply).apply);
}

static void
test_apply_takes_a_tap_once_something_is_pending(void) {
    tap_fixture(SAND_COLOUR_256, false);
    menu.draft.quality = 0;
    TEST_ASSERT_TRUE(tap(layout_for(false).apply).apply);
}

/* The DITHER row exists in the layout at every colour mode - only draw_dither()
 * decides whether anything real sits there - so a tap where it would be must
 * hit nothing, and open no list, unless the draft colour is 16. */
static void
assert_dither_dropdown_absent_for(sand_colour_mode_t colour) {
    tap_fixture(colour, false);
    const options_screen_layout_t lay = layout_for(false);

    TEST_ASSERT_FALSE_MESSAGE(any_hit(tap(lay.dither)),
                              "a tap where the dither dropdown would sit must hit nothing under this colour mode");

    const mu_Rect list = ui_dropdown_list_rect(lay.dither, DITHER_COUNT, lay.dither.h, canvas_h(false), UI_MARGIN);
    const sand_options_hits_t hits = tap_at(list.x + list.w / 2, list.y + lay.dither.h / 2);
    TEST_ASSERT_FALSE_MESSAGE(any_hit(hits), "no list can have opened, so nothing sits where its first row would");
}

static void
test_the_dither_dropdown_is_absent_unless_the_draft_colour_is_sixteen(void) {
    assert_dither_dropdown_absent_for(SAND_COLOUR_256);
    assert_dither_dropdown_absent_for(SAND_COLOUR_FULL);
}

static void
test_a_tap_at_the_sliders_right_end_reports_the_finest_quality(void) {
    tap_fixture(SAND_COLOUR_256, false);
    const mu_Rect s = layout_for(false).quality_slider;
    TEST_ASSERT_EQUAL_INT(0, tap_at(s.x + s.w - 4, s.y + s.h / 2).quality);
}

static void
test_a_tap_at_the_sliders_left_end_reports_the_coarsest_quality(void) {
    tap_fixture(SAND_COLOUR_256, false);
    const mu_Rect s = layout_for(false).quality_slider;
    TEST_ASSERT_EQUAL_INT(QUALITY_COUNT - 1, tap_at(s.x + 4, s.y + s.h / 2).quality);
}

static void
test_a_tap_on_cancel_reports_cancel_and_not_apply(void) {
    tap_fixture(SAND_COLOUR_256, false);
    const sand_options_hits_t hits = tap(layout_for(false).cancel);
    TEST_ASSERT_TRUE_MESSAGE(hits.cancel, "a tap on CANCEL must report cancel");
    TEST_ASSERT_FALSE_MESSAGE(hits.apply, "a tap on CANCEL must never also report apply");
}

/* Every tile draws a face rect exactly its own size - ui_frame_spans()'s
 * first span - so its colour says whether that tile drew selected. */
static mu_Color
tile_face_color(mu_Rect tile) {
    mu_Command* cmd = NULL;
    while (mu_next_command(ui_context(), &cmd)) {
        if (cmd->type != MU_COMMAND_RECT) {
            continue;
        }
        const mu_Rect r = cmd->rect.rect;
        if (r.x == tile.x && r.y == tile.y && r.w == tile.w && r.h == tile.h) {
            return cmd->rect.color;
        }
    }
    TEST_FAIL_MESSAGE("no face rect drawn at a tile's own rect");
    return (mu_Color){0};
}

static bool
same_color(mu_Color a, mu_Color b) {
    return a.r == b.r && a.g == b.g && a.b == b.b;
}

static void
test_the_draft_colour_tile_is_selected_even_when_uncommitted(void) {
    tap_fixture(SAND_COLOUR_256, false);
    menu.draft.color = SAND_COLOUR_FULL;
    options_frame(false, false, false, 0, 0);

    const options_screen_layout_t lay = layout_for(false);
    for (int i = 0; i < OPTIONS_SCREEN_TILE_COUNT; i++) {
        const mu_Color face = tile_face_color(lay.tiles[i]);
        const bool is_draft = options_screen_tile_colour(i) == menu.draft.color;
        if (is_draft) {
            TEST_ASSERT_TRUE_MESSAGE(same_color(face, sand_ui_theme.accent_face),
                                     "the draft colour's own tile must draw on accent_face");
        } else {
            TEST_ASSERT_FALSE_MESSAGE(same_color(face, sand_ui_theme.accent_face),
                                      "a tile that is not the draft colour must not draw on accent_face");
        }
    }
}

static void
test_the_header_is_present_in_portrait_and_absent_in_landscape(void) {
    TEST_ASSERT_GREATER_THAN_INT(0, layout_for(false).header.h);
    TEST_ASSERT_EQUAL_INT(0, layout_for(true).header.h);
}

static void
test_the_dither_dropdown_picks_from_its_list(void) {
    tap_fixture(SAND_COLOUR_16, false);
    const options_screen_layout_t lay = layout_for(false);
    TEST_ASSERT_FALSE(any_hit(tap(lay.dither)));

    const mu_Rect list = ui_dropdown_list_rect(lay.dither, DITHER_COUNT, lay.dither.h, canvas_h(false), UI_MARGIN);
    const sand_options_hits_t hits = tap_at(list.x + list.w / 2, list.y + lay.dither.h / 2);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, hits.dither, "the top row of the list is the first dither");
    TEST_ASSERT_FALSE_MESSAGE(hits.color >= 0, "the list, not a tile beneath it, takes the tap");
}

static void
test_a_landscape_tap_picks_the_open_dither_list_row(void) {
    tap_fixture(SAND_COLOUR_16, true);
    const options_screen_layout_t lay = layout_for(true);
    TEST_ASSERT_FALSE(any_hit(tap(lay.dither)));
    const mu_Rect list = ui_dropdown_list_rect(lay.dither, DITHER_COUNT, lay.dither.h, canvas_h(true), UI_MARGIN);
    const int row = 2;
    const int scroll = ui_dropdown_list_scroll(menu.draft.dither, DITHER_COUNT, lay.dither.h, list.h);
    const sand_options_hits_t hits =
        tap_at(list.x + list.w / 2, list.y + row * lay.dither.h - scroll + lay.dither.h / 2);
    TEST_ASSERT_EQUAL_INT(row, hits.dither);
    TEST_ASSERT_FALSE(hits.color >= 0);
}

static const mu_Container*
open_list(mu_Rect list) {
    for (int i = 0; i < ui_context()->root_list.idx; i++) {
        const mu_Container* c = ui_context()->root_list.items[i];
        if (c->rect.x == list.x && c->rect.y == list.y && c->rect.h == list.h) {
            return c;
        }
    }
    return NULL;
}

static void
finger_at(bool landscape, bool pressed, bool released, int x, int y) {
    int px, py;
    ui_transform_point(ui_transform_quarter_turn(landscape ? 1 : 0, GFX_WIDTH, GFX_HEIGHT), x, y, &px, &py);
    options_frame(!released, pressed, released, px, py);
}

/* Held on the bottom row, then dragged up the screen. In landscape that is a
 * sideways stroke on the panel, which must still read as a scroll. */
static void
assert_a_drag_scrolls_the_dither_list(bool landscape) {
    tap_fixture(SAND_COLOUR_16, landscape);
    menu.draft.dither = 0;
    const options_screen_layout_t lay = layout_for(landscape);
    TEST_ASSERT_FALSE(any_hit(tap(lay.dither)));
    const mu_Rect list = ui_dropdown_list_rect(lay.dither, DITHER_COUNT, lay.dither.h, canvas_h(landscape), UI_MARGIN);
    const mu_Container* cnt = open_list(list);
    TEST_ASSERT_NOT_NULL_MESSAGE(cnt, "the tap must have opened the list");
    TEST_ASSERT_LESS_THAN_INT_MESSAGE(DITHER_COUNT * lay.dither.h, list.h, "a list that fits has nothing to scroll");
    const int before = cnt->scroll.y;
    const int x = list.x + list.w / 2;
    const int y0 = list.y + list.h - lay.dither.h / 2;
    finger_at(landscape, true, false, x, y0);
    for (int i = 0; i < 3; i++) {
        finger_at(landscape, false, false, x, y0);
    }
    for (int dy = 4; dy <= 40; dy += 4) {
        finger_at(landscape, false, false, x, y0 - dy);
    }
    TEST_ASSERT_NOT_EQUAL_MESSAGE(before, cnt->scroll.y, "a finger held on the list, then dragged, scrolls it");
    finger_at(landscape, false, true, x, y0 - 40);
}

static void
test_a_drag_scrolls_the_dither_list_landscape(void) {
    assert_a_drag_scrolls_the_dither_list(true);
}

static void
test_a_drag_scrolls_the_dither_list_portrait(void) {
    assert_a_drag_scrolls_the_dither_list(false);
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
    RUN_TEST(test_a_landscape_tap_on_a_tile_reports_its_colour_mode);
    RUN_TEST(test_apply_takes_no_tap_while_nothing_is_pending);
    RUN_TEST(test_apply_takes_a_tap_once_something_is_pending);
    RUN_TEST(test_the_dither_dropdown_picks_from_its_list);
    RUN_TEST(test_a_landscape_tap_picks_the_open_dither_list_row);
    RUN_TEST(test_a_drag_scrolls_the_dither_list_landscape);
    RUN_TEST(test_a_drag_scrolls_the_dither_list_portrait);
    RUN_TEST(test_the_dither_dropdown_is_absent_unless_the_draft_colour_is_sixteen);
    RUN_TEST(test_a_tap_at_the_sliders_right_end_reports_the_finest_quality);
    RUN_TEST(test_a_tap_at_the_sliders_left_end_reports_the_coarsest_quality);
    RUN_TEST(test_a_tap_on_cancel_reports_cancel_and_not_apply);
    RUN_TEST(test_the_draft_colour_tile_is_selected_even_when_uncommitted);
    RUN_TEST(test_the_header_is_present_in_portrait_and_absent_in_landscape);
}

SUITE_REGISTER(run_options_screen_suite);
