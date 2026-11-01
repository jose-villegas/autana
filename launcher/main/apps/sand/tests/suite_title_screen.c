/*
 * Portable suite: title_screen - its geometry at both orientations, every
 * fixed string against its own rect, and a real tap through the face.
 */

#include <stdio.h>
#include <string.h>

#include "suites.h"
#include "unity.h"

#include "draw/draw_font_roles.h"
#include "gfx/gfx.h"
#include "input/input.h"
#include "ui/ui.h"
#include "ui_probe.h"

#include "apps/sand/ui/sand_theme.h"
#include "apps/sand/ui/title_screen.h"

static void
layout_for(bool landscape, title_screen_layout_t* lay) {
    const int w = landscape ? GFX_HEIGHT : GFX_WIDTH;
    const int h = landscape ? GFX_WIDTH : GFX_HEIGHT;
    title_screen_layout(w, h, lay);
}

static bool
inside(recti_t r, int w, int h) {
    return r.x >= 0 && r.y >= 0 && r.x + r.w <= w && r.y + r.h <= h;
}

static bool
overlaps(recti_t a, recti_t b) {
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

static bool
contains(recti_t outer, recti_t inner) {
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
        const recti_t b = lay.buttons[i];
        TEST_ASSERT_TRUE_MESSAGE(inside(b, w, h), title_screen_label((sand_title_button_t)i));
        TEST_ASSERT_GREATER_OR_EQUAL_INT(UI_TAP_MIN, b.w < b.h ? b.w : b.h);
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

    TEST_ASSERT_LESS_OR_EQUAL_INT(lay.header.w - (2 * UI_MARGIN),
                                  draw_font_text_width(draw_font_ui(), TITLE_SCREEN_TITLE, -1, lay.title_scale));
    TEST_ASSERT_LESS_OR_EQUAL_INT(
        lay.subtitle.w, draw_font_text_width(draw_font_ui(), TITLE_SCREEN_SUBTITLE, -1, sand_ui_theme.text_scale));
    for (int i = 0; i < SAND_TITLE_BUTTON_COUNT; i++) {
        const char* label = title_screen_label((sand_title_button_t)i);
        TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE(ui_probe_button_label_width(lay.buttons[i].w, true, &sand_ui_theme),
                                              draw_font_text_width(draw_font_ui(), label, -1, sand_ui_theme.text_scale),
                                              label);
    }
}

static void
test_every_title_string_fits_its_rect_portrait(void) {
    assert_strings_fit(false);
}

static void
test_every_title_string_fits_its_rect_landscape(void) {
    assert_strings_fit(true);
}

static void
test_the_title_grows_where_the_canvas_is_wide_enough(void) {
    title_screen_layout_t portrait;
    title_screen_layout_t landscape;
    layout_for(false, &portrait);
    layout_for(true, &landscape);
    TEST_ASSERT_GREATER_THAN_INT(portrait.title_scale, landscape.title_scale);
}

/* A real tap, portrait, through ui_begin()'s own pointer bridge. */

static sand_title_button_t
title_frame(bool down, bool pressed, bool released, vec2i_t point) {
    input_t in = {.down = down, .pressed = pressed, .released = released, .point = point};
    ui_begin(&in);
    const sand_title_button_t hit = title_screen_draw();
    ui_probe_end_frame();
    return hit;
}

static sand_title_button_t
tap(recti_t r) {
    const int x = r.x + (r.w / 2);
    const int y = r.y + (r.h / 2);
    const affine2x_t transform = ui_transform_quarter_turn(ui_width() == GFX_HEIGHT ? 1 : 0, GFX_WIDTH, GFX_HEIGHT);
    const vec2i_t mapped_pixel = affine2x_map_pixel(&transform, (vec2i_t){x, y});
    sand_title_button_t hit = SAND_TITLE_NONE;
    sand_title_button_t f = title_frame(true, true, false, mapped_pixel);
    hit = f != SAND_TITLE_NONE ? f : hit;
    for (int i = 0; i < 4; i++) {
        f = title_frame(true, false, false, mapped_pixel);
        hit = f != SAND_TITLE_NONE ? f : hit;
    }
    f = title_frame(false, false, true, mapped_pixel);
    hit = f != SAND_TITLE_NONE ? f : hit;
    for (int i = 0; i < 2; i++) {
        f = title_frame(false, false, false, mapped_pixel);
        hit = f != SAND_TITLE_NONE ? f : hit;
    }
    return hit;
}

static void
tap_fixture(bool landscape) {
    ui_init();
    ui_set_transform(ui_transform_quarter_turn(landscape ? 1 : 0, GFX_WIDTH, GFX_HEIGHT));
    title_frame(false, false, false, (vec2i_t){0, 0});
    title_frame(false, false, false, (vec2i_t){0, 0});
}

static void
test_a_tap_reports_the_button_under_it(void) {
    title_screen_layout_t lay;
    layout_for(false, &lay);

    const sand_title_button_t live[] = {SAND_TITLE_START, SAND_TITLE_OPTIONS, SAND_TITLE_EXIT};
    for (size_t i = 0; i < sizeof live / sizeof live[0]; i++) {
        tap_fixture(false);
        TEST_ASSERT_EQUAL_INT_MESSAGE(live[i], tap(lay.buttons[live[i]]), title_screen_label(live[i]));
    }
}

#define TOUCH_HOLD_FRAMES 4

/* A tap fires on the frame the touch is released, never on the press or
 * while it is held. */
static void
test_a_title_button_fires_on_release_only(void) {
    title_screen_layout_t lay;
    layout_for(false, &lay);
    const sand_title_button_t live[] = {SAND_TITLE_START, SAND_TITLE_OPTIONS, SAND_TITLE_EXIT};
    for (size_t i = 0; i < sizeof live / sizeof live[0]; i++) {
        tap_fixture(false);
        const recti_t r = lay.buttons[live[i]];
        const int x = r.x + (r.w / 2);
        const int y = r.y + (r.h / 2);
        TEST_ASSERT_EQUAL_INT_MESSAGE(SAND_TITLE_NONE, title_frame(true, true, false, (vec2i_t){x, y}),
                                      "fired on the press");
        for (int hold = 0; hold < TOUCH_HOLD_FRAMES; hold++) {
            TEST_ASSERT_EQUAL_INT_MESSAGE(SAND_TITLE_NONE, title_frame(true, false, false, (vec2i_t){x, y}),
                                          "fired while held");
        }
        TEST_ASSERT_EQUAL_INT_MESSAGE(live[i], title_frame(false, false, true, (vec2i_t){x, y}),
                                      title_screen_label(live[i]));
    }
}

static void
test_a_landscape_tap_reports_the_button_under_it(void) {
    title_screen_layout_t lay;
    layout_for(true, &lay);
    tap_fixture(true);
    TEST_ASSERT_EQUAL_INT(SAND_TITLE_START, tap(lay.buttons[SAND_TITLE_START]));
}

/* LOAD SAVES and GUIDE have no screen behind them yet, drawn disabled -
 * a disabled button is inked with the theme's muted colour, so this pins
 * that these two specific labels really do, and the three live ones don't. */
static void
test_disabled_buttons_are_inked_muted(void) {
    tap_fixture(false);
    title_frame(false, false, false, (vec2i_t){0, 0});

    for (int i = 0; i < SAND_TITLE_BUTTON_COUNT; i++) {
        const char* label = title_screen_label((sand_title_button_t)i);
        uint32_t ink = 0;
        TEST_ASSERT_TRUE_MESSAGE(ui_probe_text_rgb(label, &ink), label);
        TEST_ASSERT_EQUAL_INT_MESSAGE(!title_screen_button_enabled((sand_title_button_t)i), ink == sand_ui_theme.muted,
                                      label);
    }
    TEST_ASSERT_FALSE(title_screen_button_enabled(SAND_TITLE_LOAD));
    TEST_ASSERT_FALSE(title_screen_button_enabled(SAND_TITLE_GUIDE));
}

static void
test_a_disabled_button_blocks_a_nearby_live_target(void) {
    title_screen_layout_t lay;
    layout_for(false, &lay);

    tap_fixture(false);
    TEST_ASSERT_EQUAL_INT(SAND_TITLE_NONE, tap(lay.buttons[SAND_TITLE_LOAD]));
    tap_fixture(false);
    TEST_ASSERT_EQUAL_INT(SAND_TITLE_NONE, tap(lay.buttons[SAND_TITLE_GUIDE]));
}

void
run_title_screen_suite(void) {
    RUN_TEST(test_the_layout_fits_portrait);
    RUN_TEST(test_the_layout_fits_landscape);
    RUN_TEST(test_every_title_string_fits_its_rect_portrait);
    RUN_TEST(test_every_title_string_fits_its_rect_landscape);
    RUN_TEST(test_the_title_grows_where_the_canvas_is_wide_enough);
    RUN_TEST(test_a_tap_reports_the_button_under_it);
    RUN_TEST(test_a_title_button_fires_on_release_only);
    RUN_TEST(test_a_landscape_tap_reports_the_button_under_it);
    RUN_TEST(test_disabled_buttons_are_inked_muted);
    RUN_TEST(test_a_disabled_button_blocks_a_nearby_live_target);
}

SUITE_REGISTER(run_title_screen_suite);
