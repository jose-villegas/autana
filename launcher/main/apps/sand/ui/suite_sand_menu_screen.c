/*
 * Portable suite: sand_menu_screen's row geometry, and the scrolling that
 * geometry now feeds into.
 *
 * sand_menu_screen_row_rect() is checked directly at row counts 3-6 - past
 * what show_dither/CONFIG_LAUNCHER_DEVELOPMENT ever combine to draw on a
 * host build, which is the only way to prove the FORMULA holds before a
 * sixth row ever ships. The reachability tests then drive real microui
 * through ui_pointer_step(), the same bridge suite_ui_pointer_microui.c
 * proves against a plain list, to show this screen's rows actually reach
 * that container's scrolling rather than merely computing rects that would.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "suites.h"
#include "unity.h"

#include "gfx/gfx.h"
#include "microui.h"
#include "ui/ui.h"
#include "ui/ui_pointer.h"
#include "ui/ui_transform.h"

#include "sand_menu_screen.h"

static void
set_portrait(void) {
    ui_set_transform(ui_transform_identity());
}

static void
set_landscape(void) {
    ui_set_transform(ui_transform_quarter_turn(1, GFX_WIDTH, GFX_HEIGHT));
}

/* Geometry, both orientations, rows 3-6. */

static void
assert_rows_fit_horizontally_at_full_height(int rows) {
    for (int row = 0; row < rows; row++) {
        const mu_Rect r = sand_menu_screen_row_rect(row, rows);
        TEST_ASSERT_EQUAL_INT_MESSAGE(UI_ROW_HEIGHT, r.h, "a row must never shrink to fit");
        TEST_ASSERT_TRUE(r.x >= 0);
        TEST_ASSERT_TRUE(r.x + r.w <= ui_width());
    }
}

static void
test_rows_stay_full_height_and_inside_the_screen_portrait(void) {
    set_portrait();
    for (int rows = 3; rows <= 6; rows++) {
        assert_rows_fit_horizontally_at_full_height(rows);
    }
}

static void
test_rows_stay_full_height_and_inside_the_screen_landscape(void) {
    set_landscape();
    for (int rows = 3; rows <= 6; rows++) {
        assert_rows_fit_horizontally_at_full_height(rows);
    }
}

static int
row_gap(int rows, int row) {
    const mu_Rect a = sand_menu_screen_row_rect(row, rows);
    const mu_Rect b = sand_menu_screen_row_rect(row + 1, rows);
    return b.y - (a.y + a.h);
}

static void
test_the_row_gap_does_not_shrink_as_row_count_grows(void) {
    set_landscape(); /* the short side - what the old shrinking gap was for */
    const int gap_at_3 = row_gap(3, 0);
    for (int rows = 4; rows <= 6; rows++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(gap_at_3, row_gap(rows, 0), "a taller menu must keep the same row gap");
    }
}

static void
test_five_and_six_rows_overflow_the_landscape_screen_at_rest(void) {
    set_landscape();
    for (int rows = 5; rows <= 6; rows++) {
        const mu_Rect last = sand_menu_screen_row_rect(rows - 1, rows);
        TEST_ASSERT_GREATER_THAN_INT_MESSAGE(ui_height(), last.y + last.h,
                                             "this overflow is exactly what scrolling now exists to reach");
    }
}

/* The real sand_menu_screen_draw(): proves ITS rows, not just the rect
 * formula, reach the container's own content height. */

static void
menu_draw_fixture(void) {
    ui_init();
    set_landscape();
}

static int
menu_content_height(bool show_dither) {
    const sand_menu_screen_state_t state = {
        .quality = "QUALITY: VERY LOW",
        .color = "COLOUR: FULL",
        .dither = "DITHER: PIXEL CHECKER2",
        .show_dither = show_dither,
    };
    const input_t input = {0};
    ui_begin(&input);
    sand_menu_screen_draw(ui_context(), &state);
    mu_end(ui_context());
    return mu_get_container(ui_context(), "Sand Menu")->content_size.y;
}

static void
test_the_real_screen_reports_its_rows_in_content_size(void) {
    menu_draw_fixture();
    const int three_rows = menu_content_height(false);
    const int four_rows = menu_content_height(true);

    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(
        UI_ROW_HEIGHT * 2, three_rows,
        "an ABSOLUTE row rect never reaches content_size at all - this is what tells the "
        "container there is anything to scroll, and a regression here reports far less than this");
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(three_rows, four_rows, "a fourth row (DITHER shown) must grow it");
}

/* Reachability: real microui, real ui_pointer, six of this screen's own
 * rows - enough to overflow landscape (see the overflow test above) - laid
 * out the same way sand_menu_screen_draw() lays out any other row count. */

#define MENU_ROWS 6

static mu_Context* ctx;
static ui_pointer_t pointer;

static int
stub_text_width(mu_Font font, const char* str, int len) {
    (void)font;
    return (len < 0 ? (int)strlen(str) : len) * 8;
}

static int
stub_text_height(mu_Font font) {
    (void)font;
    return 8;
}

static void
fixture(void) {
    TEST_ASSERT_NOT_NULL(ctx);
    memset(ctx, 0, sizeof *ctx);
    memset(&pointer, 0, sizeof pointer);
    mu_init(ctx);
    ctx->text_width = stub_text_width;
    ctx->text_height = stub_text_height;
    set_landscape();
}

static mu_Rect
relative_row_rect(int row) {
    const mu_Rect rest = sand_menu_screen_row_rect(row, MENU_ROWS);
    const int p = ctx->style->padding;
    return (mu_Rect){rest.x - p, rest.y - p, rest.w, rest.h};
}

/* One frame of the real bridge, same shape as sand_menu_screen_draw()'s own
 * window, over MENU_ROWS of this screen's real row geometry. Returns which
 * row submitted this frame, or -1. */
static int
menu_frame(bool down, bool pressed, bool released, int x, int y) {
    input_t in = {0};
    in.down = down;
    in.pressed = pressed;
    in.released = released;
    in.x = x;
    in.y = y;

    ui_pointer_event_t ev[UI_POINTER_MAX_EVENTS];
    const int n = ui_pointer_step(&pointer, &in, ev, UI_POINTER_MAX_EVENTS);
    for (int i = 0; i < n; i++) {
        switch (ev[i].kind) {
            case UI_POINTER_MOVE: mu_input_mousemove(ctx, ev[i].x, ev[i].y); break;
            case UI_POINTER_DOWN: mu_input_mousedown(ctx, ev[i].x, ev[i].y, MU_MOUSE_LEFT); break;
            case UI_POINTER_UP: mu_input_mouseup(ctx, ev[i].x, ev[i].y, MU_MOUSE_LEFT); break;
            case UI_POINTER_SCROLL: mu_input_scroll(ctx, ev[i].x, ev[i].y); break;
        }
    }

    int submitted = -1;
    mu_begin(ctx);
    if (mu_begin_window_ex(ctx, "Sand Menu", mu_rect(0, 0, ui_width(), ui_height()),
                           MU_OPT_NOTITLE | MU_OPT_NORESIZE | MU_OPT_NOCLOSE | MU_OPT_NOFRAME)) {
        for (int row = 0; row < MENU_ROWS; row++) {
            mu_layout_set_next(ctx, relative_row_rect(row), 1);
            char label[8];
            snprintf(label, sizeof label, "R%d", row);
            if (mu_button(ctx, label)) {
                submitted = row;
            }
        }
        mu_end_window(ctx);
    }
    mu_end(ctx);
    pointer.over_scrollable = ctx->scroll_target != NULL;
    return submitted;
}

static void
menu_idle_frames(int count) {
    for (int i = 0; i < count; i++) {
        menu_frame(false, false, false, 0, 0);
    }
}

static int
menu_drag(int x, int y0, int y1, int steps) {
    int submits = 0;
    submits += menu_frame(true, true, false, x, y0) >= 0;
    for (int i = 1; i <= steps; i++) {
        submits += menu_frame(true, false, false, x, y0 + (y1 - y0) * i / steps) >= 0;
    }
    submits += menu_frame(false, false, true, x, y1) >= 0;
    return submits;
}

static int
menu_tap(int x, int y) {
    int row = -1;
    int r = menu_frame(true, true, false, x, y);
    row = r >= 0 ? r : row;
    for (int i = 0; i < 4; i++) {
        r = menu_frame(true, false, false, x, y);
        row = r >= 0 ? r : row;
    }
    r = menu_frame(false, false, true, x, y);
    return r >= 0 ? r : row;
}

static void
test_a_tap_on_the_menu_presses_the_row_under_it_once(void) {
    fixture();
    menu_idle_frames(2);

    const mu_Rect first = sand_menu_screen_row_rect(0, MENU_ROWS);
    TEST_ASSERT_EQUAL_INT(0, menu_tap(first.x + first.w / 2, first.y + first.h / 2));
}

static void
test_the_last_row_is_reachable_by_scrolling_once_six_rows_overflow(void) {
    fixture();
    menu_idle_frames(2);

    for (int i = 0; i < 6; i++) {
        menu_drag(ui_width() / 2, ui_height() - 20, 20, 8);
        menu_idle_frames(1);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(MENU_ROWS - 1, menu_tap(ui_width() / 2, ui_height() - 40),
                                  "the bottom of the glass must now hold the last row");
}

void
run_sand_menu_screen_suite(void) {
    RUN_TEST(test_rows_stay_full_height_and_inside_the_screen_portrait);
    RUN_TEST(test_rows_stay_full_height_and_inside_the_screen_landscape);
    RUN_TEST(test_the_row_gap_does_not_shrink_as_row_count_grows);
    RUN_TEST(test_five_and_six_rows_overflow_the_landscape_screen_at_rest);
    RUN_TEST(test_the_real_screen_reports_its_rows_in_content_size);

    ctx = malloc(sizeof *ctx);
    RUN_TEST(test_a_tap_on_the_menu_presses_the_row_under_it_once);
    RUN_TEST(test_the_last_row_is_reachable_by_scrolling_once_six_rows_overflow);
    free(ctx);
    ctx = NULL;
}

SUITE_REGISTER(run_sand_menu_screen_suite);
