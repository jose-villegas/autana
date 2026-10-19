/*
 * Host-only suite: the home screen's own row placement, flowed in a face
 * scroll window, so rows past the screen stay reachable by scrolling.
 *
 * Drives the real ui_launcher_draw() between ui_begin() and
 * ui_end_commands(); ui_end() alone needs the real framebuffer, which is why
 * ui_launcher_draw() was split out from it in the first place (see
 * ui_launcher.c's own top comment).
 *
 * Registers its fixture apps through the real app_register() and clears
 * them with app_registry_reset_for_test(), which no device build has;
 * hence run_tests.sh only.
 */

#include "suites.h"

#ifndef DEVICE_BUILD

#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "app/app.h"
#include "gfx/gfx.h"
#include "shell/ui/ui_launcher.h"
#include "test_cleanup.h"
#include "ui/layout/ui_transform.h"
#include "ui/ui.h"
#include "ui/ui_internal.h"
#include "ui_probe.h"

/* LAUNCHER_BTN_W (ui_launcher_draw.c), private to that file, mirrored here so
 * this suite can tap the same physical spot a real finger would. */
#define LAUNCHER_BTN_W 240

#define MAX_TEST_APPS  20

static app_t test_apps[MAX_TEST_APPS];
static char test_app_names[MAX_TEST_APPS][8];

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
 * agree: test_apps[i] lands at row i either way. */
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

static void
set_landscape(void) {
    ui_set_transform(ui_transform_quarter_turn(1, GFX_WIDTH, GFX_HEIGHT));
}

static void
fixture(int app_count) {
    ui_init();
    set_landscape();
    set_app_count(app_count);
}

/* One frame of the real launcher rows, built but not painted, with the
 * finger at logical (`x`, `y`) mapped onto the panel the way a real touch
 * arrives. Returns the chosen app, or NULL. */
static const app_t*
launcher_frame(bool down, bool pressed, bool released, int x, int y, uint32_t dt_ms) {
    input_t in = {0};
    in.down = down;
    in.pressed = pressed;
    in.released = released;
    const affine2x_t t = ui_effective_transform();
    const vec2i_t panel = affine2x_map_pixel(&t, (vec2i_t){x, y});
    in.x = panel.x;
    in.y = panel.y;

    (void)dt_ms;

    ui_begin(&in);
    const app_t* chosen = ui_launcher_draw();
    ui_end_commands();
    return chosen;
}

static void
idle_frames(int count) {
    for (int i = 0; i < count; i++) {
        launcher_frame(false, false, false, 0, 0, 16);
    }
}

static const app_t*
tap(int x, int y) {
    const app_t* chosen = NULL;
    const app_t* r = launcher_frame(true, true, false, x, y, 16);
    chosen = r != NULL ? r : chosen;
    for (int i = 0; i < 4; i++) {
        r = launcher_frame(true, false, false, x, y, 16);
        chosen = r != NULL ? r : chosen;
    }
    r = launcher_frame(false, false, true, x, y, 16);
    return r != NULL ? r : chosen;
}

static int
drag(int x, int y0, int y1, int steps) {
    int taps = 0;
    taps += launcher_frame(true, true, false, x, y0, 16) != NULL;
    for (int i = 1; i <= steps; i++) {
        taps += launcher_frame(true, false, false, x, y0 + (y1 - y0) * i / steps, 16) != NULL;
    }
    taps += launcher_frame(false, false, true, x, y1, 16) != NULL;
    return taps;
}

/* Row i sits UI_BANNER_HEIGHT + UI_ROW_GAP down, then UI_ROW_HEIGHT +
 * UI_ROW_GAP per row after that, centred. */
static recti_t
old_row_rect(int i) {
    const int y = UI_BANNER_HEIGHT + UI_ROW_GAP + i * (UI_ROW_HEIGHT + UI_ROW_GAP);
    return ui_centered_rect(ui_width(), LAUNCHER_BTN_W, UI_ROW_HEIGHT, y);
}

static void
test_rows_land_at_todays_positions_when_nothing_overflows(void) {
    fixture(3);
    suite_set_test_cleanup(ui_probe_widgets_stop);
    ui_probe_widgets_start();
    idle_frames(2);

    for (int i = 0; i < 3; i++) {
        const recti_t r = old_row_rect(i);
        recti_t drawn;
        TEST_ASSERT_TRUE(ui_probe_widget(test_app_names[i], &drawn));
        TEST_ASSERT_EQUAL_MEMORY_MESSAGE(&r, &drawn, sizeof r, "a non-overflowing row must land exactly here");
        TEST_ASSERT_EQUAL_PTR_MESSAGE(&test_apps[i], tap(r.x + r.w / 2, r.y + r.h / 2),
                                      "a non-overflowing list must keep every row exactly where it was");
    }
}

static void
test_the_last_app_is_reachable_once_enough_apps_overflow(void) {
    fixture(MAX_TEST_APPS);
    idle_frames(2);

    for (int i = 0; i < 8; i++) {
        drag(ui_width() / 2, ui_height() - 20, 20, 8);
        idle_frames(1);
    }

    TEST_ASSERT_EQUAL_PTR_MESSAGE(&test_apps[MAX_TEST_APPS - 1], tap(ui_width() / 2, ui_height() - 40),
                                  "enough apps must now scroll into reach instead of falling off the bottom");
}

void
run_ui_launcher_suite(void) {
    RUN_TEST(test_rows_land_at_todays_positions_when_nothing_overflows);
    RUN_TEST(test_the_last_app_is_reachable_once_enough_apps_overflow);
}

#else

void
run_ui_launcher_suite(void) {}

#endif

SUITE_REGISTER(run_ui_launcher_suite);
