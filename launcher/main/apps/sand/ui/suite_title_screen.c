/*
 * Portable suite: title_screen - its geometry at both orientations, every
 * fixed string against its own rect, and a real tap through microui.
 */

#include <stdio.h>

#include "suites.h"
#include "unity.h"

#include "gfx/gfx.h"
#include "gfx/gfx_font_roles.h"
#include "input/input.h"
#include "ui/ui.h"
#include "ui/ui_transform.h"

#include "sand_theme.h"
#include "title_screen.h"

#define TAP_TARGET_MIN 44

static void
layout_for(bool landscape, title_screen_layout_t* lay) {
    const int w = landscape ? GFX_HEIGHT : GFX_WIDTH;
    const int h = landscape ? GFX_WIDTH : GFX_HEIGHT;
    title_screen_layout(w, h, lay);
}

static bool
inside(mu_Rect r, int w, int h) {
    return r.x >= 0 && r.y >= 0 && r.x + r.w <= w && r.y + r.h <= h;
}

static bool
overlaps(mu_Rect a, mu_Rect b) {
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

static bool
contains(mu_Rect outer, mu_Rect inner) {
    return inner.x >= outer.x && inner.y >= outer.y && inner.x + inner.w <= outer.x + outer.w
           && inner.y + inner.h <= outer.y + outer.h;
}

static void
assert_layout(bool landscape) {
    const int w = landscape ? GFX_HEIGHT : GFX_WIDTH;
    const int h = landscape ? GFX_WIDTH : GFX_HEIGHT;
    title_screen_layout_t lay;
    layout_for(landscape, &lay);

    TEST_ASSERT_TRUE(inside(lay.header, w, h));
    TEST_ASSERT_TRUE(inside(lay.subtitle, w, h));
    TEST_ASSERT_TRUE(inside(lay.footer, w, h));
    TEST_ASSERT_FALSE(overlaps(lay.header, lay.subtitle));

    for (int i = 0; i < SAND_TITLE_BUTTON_COUNT; i++) {
        const mu_Rect b = lay.buttons[i];
        TEST_ASSERT_TRUE_MESSAGE(inside(b, w, h), title_screen_label((sand_title_button_t)i));
        TEST_ASSERT_GREATER_OR_EQUAL_INT(TAP_TARGET_MIN, b.h);
        TEST_ASSERT_FALSE(overlaps(b, lay.header));
        TEST_ASSERT_FALSE(overlaps(b, lay.subtitle));
        for (int j = i + 1; j < SAND_TITLE_BUTTON_COUNT; j++) {
            TEST_ASSERT_FALSE_MESSAGE(overlaps(b, lay.buttons[j]), title_screen_label((sand_title_button_t)i));
        }
    }

    TEST_ASSERT_TRUE(contains(lay.footer, lay.buttons[SAND_TITLE_GUIDE]));
    TEST_ASSERT_TRUE(contains(lay.footer, lay.buttons[SAND_TITLE_EXIT]));
    for (int i = SAND_TITLE_START; i <= SAND_TITLE_OPTIONS; i++) {
        TEST_ASSERT_FALSE(overlaps(lay.buttons[i], lay.footer));
    }
}

static void
test_the_layout_fits_portrait(void) {
    assert_layout(false);
}

static void
test_the_layout_fits_landscape(void) {
    assert_layout(true);
}

static void
assert_strings_fit(bool landscape) {
    title_screen_layout_t lay;
    layout_for(landscape, &lay);

    TEST_ASSERT_LESS_OR_EQUAL_INT(lay.header.w - 2 * UI_MARGIN,
                                  gfx_font_text_width(gfx_font_ui(), TITLE_SCREEN_TITLE, -1, lay.title_scale));
    TEST_ASSERT_LESS_OR_EQUAL_INT(
        lay.subtitle.w, gfx_font_text_width(gfx_font_ui(), TITLE_SCREEN_SUBTITLE, -1, sand_ui_theme.text_scale));
    for (int i = 0; i < SAND_TITLE_BUTTON_COUNT; i++) {
        const char* label = title_screen_label((sand_title_button_t)i);
        TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE(ui_icon_button_label_width(lay.buttons[i].w, true, &sand_ui_theme),
                                              gfx_font_text_width(gfx_font_ui(), label, -1, sand_ui_theme.text_scale),
                                              label);
    }
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
test_the_title_grows_where_the_canvas_is_wide_enough(void) {
    title_screen_layout_t portrait, landscape;
    layout_for(false, &portrait);
    layout_for(true, &landscape);
    TEST_ASSERT_GREATER_THAN_INT(portrait.title_scale, landscape.title_scale);
}

/* A real tap, portrait, through ui_begin()'s own pointer bridge. */

static sand_title_button_t
title_frame(bool down, bool pressed, bool released, int x, int y) {
    input_t in = {.down = down, .pressed = pressed, .released = released, .x = x, .y = y};
    ui_begin(&in);
    const sand_title_button_t hit = title_screen_draw(ui_context());
    mu_end(ui_context());
    return hit;
}

static sand_title_button_t
tap(mu_Rect r) {
    const int x = r.x + r.w / 2;
    const int y = r.y + r.h / 2;
    sand_title_button_t hit = SAND_TITLE_NONE;
    sand_title_button_t f = title_frame(true, true, false, x, y);
    hit = f != SAND_TITLE_NONE ? f : hit;
    for (int i = 0; i < 4; i++) {
        f = title_frame(true, false, false, x, y);
        hit = f != SAND_TITLE_NONE ? f : hit;
    }
    f = title_frame(false, false, true, x, y);
    hit = f != SAND_TITLE_NONE ? f : hit;
    for (int i = 0; i < 2; i++) {
        f = title_frame(false, false, false, x, y);
        hit = f != SAND_TITLE_NONE ? f : hit;
    }
    return hit;
}

static void
tap_fixture(void) {
    ui_init();
    ui_set_transform(ui_transform_identity());
    title_frame(false, false, false, 0, 0);
    title_frame(false, false, false, 0, 0);
}

static void
test_a_tap_reports_the_button_under_it(void) {
    title_screen_layout_t lay;
    layout_for(false, &lay);

    const sand_title_button_t live[] = {SAND_TITLE_START, SAND_TITLE_OPTIONS, SAND_TITLE_EXIT};
    for (size_t i = 0; i < sizeof live / sizeof live[0]; i++) {
        tap_fixture();
        TEST_ASSERT_EQUAL_INT_MESSAGE(live[i], tap(lay.buttons[live[i]]), title_screen_label(live[i]));
    }
}

static void
test_a_button_with_nothing_behind_it_takes_no_tap(void) {
    title_screen_layout_t lay;
    layout_for(false, &lay);

    tap_fixture();
    TEST_ASSERT_EQUAL_INT(SAND_TITLE_NONE, tap(lay.buttons[SAND_TITLE_LOAD]));
    tap_fixture();
    TEST_ASSERT_EQUAL_INT(SAND_TITLE_NONE, tap(lay.buttons[SAND_TITLE_GUIDE]));
}

void
run_title_screen_suite(void) {
    RUN_TEST(test_the_layout_fits_portrait);
    RUN_TEST(test_the_layout_fits_landscape);
    RUN_TEST(test_every_string_fits_its_rect_portrait);
    RUN_TEST(test_every_string_fits_its_rect_landscape);
    RUN_TEST(test_the_title_grows_where_the_canvas_is_wide_enough);
    RUN_TEST(test_a_tap_reports_the_button_under_it);
    RUN_TEST(test_a_button_with_nothing_behind_it_takes_no_tap);
}

SUITE_REGISTER(run_title_screen_suite);
