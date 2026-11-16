/*
 * The home screen's grid: its shape held either way, its place on the panel,
 * its taps and its scroll past one screen. The layout half is host-only: it
 * registers fixture apps through the real app_register() and clears them with
 * app_registry_reset_for_test(), which no device build has. The device half
 * checks the apps a real image registers: every name fits its tile.
 */

#include "suites.h"

#ifndef DEVICE_BUILD

#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "app/app.h"
#include "display/display.h"
#include "gfx/gfx.h"
#include "shell/ui/ui_launcher.h"
#include "testkit/cleanup.h"
#include "ui/layout/ui_transform.h"
#include "ui/ui.h"
#include "ui/ui_internal.h"
#include "ui_probe.h"

#define MAX_TEST_APPS     20
/* One screen of the grid either way: three by two, or two by three. */
#define ONE_SCREEN_APPS   6
#define COLS_LANDSCAPE    3
#define COLS_PORTRAIT     2
#define QUARTER_PORTRAIT  0
#define QUARTER_LANDSCAPE 1
/* Fewer apps than one row of either shape fills. */
#define SHORT_GRID_APPS   3
/* Two frames: the grid centres itself from the frame before. */
#define SETTLE_FRAMES     2
#define TAP_HOLD_FRAMES   4
#define SCROLL_DRAGS      8
#define DRAG_STEPS        8
/* Where a drag starts and ends, in from the canvas's top and bottom. */
#define DRAG_MARGIN       20

static app_t test_apps[MAX_TEST_APPS];
static char test_app_names[MAX_TEST_APPS][8];
static int test_quarter;

static void
noop_enter(void) {}

static void
noop_frame(uint32_t dt_ms, const input_t* input) {
    (void)dt_ms;
    (void)input;
}

static void
noop_exit(void) {}

/* Zero-padded so ascending index order and the registry's own name order
 * agree: test_apps[i] is the i-th tile either way. */
static void
set_app_count(int n) {
    TEST_ASSERT_TRUE(n <= MAX_TEST_APPS);
    app_registry_reset_for_test();
    for (int i = 0; i < n; i++) {
        snprintf(test_app_names[i], sizeof test_app_names[i], "App%02d", i);
        test_apps[i] = (app_t){
            .name = test_app_names[i],
            .enter = noop_enter,
            .frame = noop_frame,
            .exit = noop_exit,
        };
        app_register(&test_apps[i]);
    }
}

static affine2x_t
turn(void) {
    return ui_transform_quarter_turn(test_quarter, GFX_WIDTH, GFX_HEIGHT);
}

static void
fixture(int app_count, int quarter) {
    suite_set_test_cleanup(ui_probe_widgets_stop);
    ui_launcher_init();
    test_quarter = quarter;
    ui_set_transform(turn());
    set_app_count(app_count);
    ui_probe_widgets_start();
}

/* One frame of the real home, built but not painted, with the finger at
 * logical (`x`, `y`) mapped onto the panel the way a real touch arrives.
 * Returns the chosen app, or NULL. */
static const app_t*
launcher_frame(bool down, bool pressed, bool released, int x, int y) {
    input_t in = {0};
    in.down = down;
    in.pressed = pressed;
    in.released = released;
    const affine2x_t t = turn();
    in.point = affine2x_map_pixel(&t, (vec2i_t){x, y});

    ui_begin(&in);
    const app_t* chosen = ui_launcher_draw();
    ui_probe_end_frame();
    return chosen;
}

static void
idle_frames(int count) {
    for (int i = 0; i < count; i++) {
        launcher_frame(false, false, false, 0, 0);
    }
}

static const app_t*
tap(int x, int y) {
    const app_t* chosen = launcher_frame(true, true, false, x, y);
    for (int i = 0; i < TAP_HOLD_FRAMES; i++) {
        const app_t* r = launcher_frame(true, false, false, x, y);
        chosen = r != NULL ? r : chosen;
    }
    const app_t* r = launcher_frame(false, false, true, x, y);
    return r != NULL ? r : chosen;
}

static void
drag(int x, int y0, int y1, int steps) {
    launcher_frame(true, true, false, x, y0);
    for (int i = 1; i <= steps; i++) {
        launcher_frame(true, false, false, x, y0 + (y1 - y0) * i / steps);
    }
    launcher_frame(false, false, true, x, y1);
}

static recti_t
tile(int i) {
    recti_t r;
    TEST_ASSERT_TRUE_MESSAGE(ui_probe_widget(test_app_names[i], &r), "every app draws a tile");
    return r;
}

/* The tiles of `count` apps: one size, row-major `cols` across, below the
 * banner and inside the panel's safe inset, centred across the canvas, each
 * at least the recommended tap size, and each tap choosing its own app. */
static void
assert_grid(int count, int cols) {
    const recti_t first = tile(0);
    int right = 0;
    for (int i = 0; i < count; i++) {
        const recti_t r = tile(i);
        TEST_ASSERT_EQUAL_INT_MESSAGE(first.w, r.w, "tiles are one size");
        TEST_ASSERT_EQUAL_INT_MESSAGE(first.h, r.h, "tiles are one size");
        TEST_ASSERT_GREATER_OR_EQUAL_INT(UI_TAP_RECOMMENDED, r.w);
        TEST_ASSERT_GREATER_OR_EQUAL_INT(UI_TAP_RECOMMENDED, r.h);
        TEST_ASSERT_EQUAL_INT_MESSAGE(tile(i - (i % cols)).y, r.y, "a row shares its top");
        TEST_ASSERT_EQUAL_INT_MESSAGE(tile(i % cols).x, r.x, "a column shares its left");
        if (i % cols > 0) {
            TEST_ASSERT_GREATER_THAN_INT(recti_right(tile(i - 1)), r.x);
        }
        if (i >= cols) {
            TEST_ASSERT_GREATER_THAN_INT(recti_bottom(tile(i - cols)), r.y);
        }
        TEST_ASSERT_GREATER_OR_EQUAL_INT(DISPLAY_PANEL_SAFE_INSET, r.x);
        TEST_ASSERT_GREATER_OR_EQUAL_INT(UI_BANNER_HEIGHT, r.y);
        TEST_ASSERT_LESS_OR_EQUAL_INT(ui_width() - DISPLAY_PANEL_SAFE_INSET, recti_right(r));
        TEST_ASSERT_LESS_OR_EQUAL_INT(ui_height() - DISPLAY_PANEL_SAFE_INSET, recti_bottom(r));
        right = recti_right(r) > right ? recti_right(r) : right;
    }
    TEST_ASSERT_INT_WITHIN_MESSAGE(1, first.x, ui_width() - right, "the grid is centred across");
    for (int i = 0; i < count; i++) {
        const recti_t r = tile(i);
        TEST_ASSERT_EQUAL_PTR_MESSAGE(&test_apps[i], tap(r.x + r.w / 2, r.y + r.h / 2),
                                      "a tap on a tile chooses that tile's app");
    }
}

static void
test_six_apps_are_three_across_two_down_held_landscape(void) {
    fixture(ONE_SCREEN_APPS, QUARTER_LANDSCAPE);
    idle_frames(SETTLE_FRAMES);
    assert_grid(ONE_SCREEN_APPS, COLS_LANDSCAPE);
    TEST_ASSERT_GREATER_THAN_INT(tile(0).y, tile(COLS_LANDSCAPE).y);
}

static void
test_six_apps_are_two_across_three_down_held_portrait(void) {
    fixture(ONE_SCREEN_APPS, QUARTER_PORTRAIT);
    idle_frames(SETTLE_FRAMES);
    assert_grid(ONE_SCREEN_APPS, COLS_PORTRAIT);
    TEST_ASSERT_GREATER_THAN_INT(tile(COLS_PORTRAIT).y, tile(2 * COLS_PORTRAIT).y);
}

/* The band the grid's rows share: below the banner and the window's
 * padding, above the panel's safe inset and the window's padding. */
static void
grid_band(int* top, int* bottom) {
    *top = UI_BANNER_HEIGHT + UI_PADDING;
    *bottom = ui_height() - DISPLAY_PANEL_SAFE_INSET;
}

/* Fewer rows than a screen holds sit centred down it, once settled. */
static void
test_fewer_apps_sit_centred_down_the_screen(void) {
    const int quarters[] = {QUARTER_PORTRAIT, QUARTER_LANDSCAPE};
    for (size_t q = 0; q < sizeof quarters / sizeof quarters[0]; q++) {
        fixture(SHORT_GRID_APPS, quarters[q]);
        idle_frames(SETTLE_FRAMES);
        int top;
        int bottom;
        grid_band(&top, &bottom);
        const int above = tile(0).y - top;
        const int below = bottom - recti_bottom(tile(SHORT_GRID_APPS - 1));
        TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, above, "a short grid does not sit at the top");
        TEST_ASSERT_INT_WITHIN_MESSAGE(1, above, below, "a short grid is centred down the screen");
        ui_probe_widgets_stop();
    }
}

/* Past one screen the grid starts at the top and scrolls. */
static void
test_a_seventh_app_starts_the_grid_at_the_top(void) {
    fixture(ONE_SCREEN_APPS + 1, QUARTER_LANDSCAPE);
    idle_frames(SETTLE_FRAMES);
    int top;
    int bottom;
    grid_band(&top, &bottom);
    TEST_ASSERT_EQUAL_INT(top, tile(0).y);
}

/* A tile's face: the one sprite face command at its rect. */
static const sprite_t*
tile_face(recti_t r) {
    mu_Command* cmd = NULL;
    while (mu_next_command(ui_context(), &cmd)) {
        const ui_sprite_command_t* face = (const ui_sprite_command_t*)cmd;
        if (cmd->type == UI_COMMAND_SPRITE && memcmp(&face->rect, &r, sizeof r) == 0) {
            return face->sprite;
        }
    }
    TEST_FAIL_MESSAGE("a home tile is drawn from the engine pack's tile skin");
    return NULL;
}

/* A held tile takes the pressed face; its neighbours keep theirs. */
static void
test_a_held_tile_takes_the_pressed_face_and_its_neighbours_do_not(void) {
    fixture(SHORT_GRID_APPS, QUARTER_LANDSCAPE);
    idle_frames(SETTLE_FRAMES);
    const sprite_t* rest = tile_face(tile(0));
    TEST_ASSERT_EQUAL_PTR(rest, tile_face(tile(1)));

    const recti_t held = tile(1);
    for (int i = 0; i <= TAP_HOLD_FRAMES; i++) {
        launcher_frame(true, i == 0, false, held.x + held.w / 2, held.y + held.h / 2);
    }
    TEST_ASSERT_TRUE_MESSAGE(tile_face(tile(1)) != rest, "the held tile takes the pressed face");
    TEST_ASSERT_EQUAL_PTR_MESSAGE(rest, tile_face(tile(0)), "its neighbour keeps its face");
    TEST_ASSERT_EQUAL_PTR_MESSAGE(rest, tile_face(tile(2)), "its neighbour keeps its face");
}

static void
test_the_last_app_is_reachable_once_enough_apps_overflow(void) {
    fixture(MAX_TEST_APPS, QUARTER_LANDSCAPE);
    idle_frames(SETTLE_FRAMES);

    for (int i = 0; i < SCROLL_DRAGS; i++) {
        drag(ui_width() / 2, ui_height() - DRAG_MARGIN, DRAG_MARGIN, DRAG_STEPS);
        idle_frames(1);
    }

    const recti_t last = tile(MAX_TEST_APPS - 1);
    TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE(ui_height(), recti_bottom(last), "the last row scrolls fully on screen");
    TEST_ASSERT_EQUAL_PTR_MESSAGE(&test_apps[MAX_TEST_APPS - 1], tap(last.x + last.w / 2, last.y + last.h / 2),
                                  "enough apps must scroll into reach instead of falling off the bottom");
}

void
run_ui_launcher_suite(void) {
    RUN_TEST(test_six_apps_are_three_across_two_down_held_landscape);
    RUN_TEST(test_six_apps_are_two_across_three_down_held_portrait);
    RUN_TEST(test_fewer_apps_sit_centred_down_the_screen);
    RUN_TEST(test_a_seventh_app_starts_the_grid_at_the_top);
    RUN_TEST(test_a_held_tile_takes_the_pressed_face_and_its_neighbours_do_not);
    RUN_TEST(test_the_last_app_is_reachable_once_enough_apps_overflow);
}

#else

#include "unity.h"

#include "gfx/gfx.h"
#include "shell/ui/ui_launcher.h"
#include "ui/layout/ui_transform.h"
#include "ui/ui.h"
#include "ui/ui_internal.h"

/* Every app this image registers, held either way: no name is wider than
 * its tile, even split over two lines. */
static void
test_every_registered_name_fits_its_tile(void) {
    for (int quarter = 0; quarter < 2; quarter++) {
        ui_set_transform(ui_transform_quarter_turn(quarter, GFX_WIDTH, GFX_HEIGHT));
        ui_begin(NULL);
        (void)ui_launcher_draw();
        ui_end_commands();
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, ui_clipped_text(), quarter != 0 ? "landscape" : "portrait");
    }
    ui_set_transform(affine2x_identity());
    ui_invalidate();
}

void
run_ui_launcher_suite(void) {
    RUN_TEST(test_every_registered_name_fits_its_tile);
}

#endif

SUITE_REGISTER(run_ui_launcher_suite);
