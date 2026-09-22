/*
 * Host-only suite: the home screen's own row placement, built on
 * ui_flow_row(), so rows past the screen stay reachable by scrolling.
 *
 * Drives the real ui_launcher_draw() through ui_pointer_step(), the same
 * bridge suite_ui_pointer_microui.c proves against a plain list, with this
 * suite's own mu_begin()/mu_end() standing in for ui_begin()/ui_end() -
 * ui_end() alone needs the real framebuffer, which is why ui_launcher_draw()
 * was split out from it in the first place (see ui_launcher.c's own top
 * comment).
 *
 * Registers its fixture apps through the real app_register() and clears
 * them with app_registry_reset_for_test(), which no device build has -
 * hence run_tests.sh only.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "suites.h"
#include "unity.h"

#include "app.h"
#include "gfx/gfx.h"
#include "microui.h"
#include "ui/ui.h"
#include "ui/ui_launcher.h"
#include "ui/ui_pointer.h"
#include "ui/ui_transform.h"

/* LAUNCHER_BTN_W (ui_launcher.c) - private to that file, mirrored here so
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
 * agree - test_apps[i] lands at row i either way. */
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
set_landscape(void) {
    ui_set_transform(ui_transform_quarter_turn(1, GFX_WIDTH, GFX_HEIGHT));
}

static void
fixture(int app_count) {
    TEST_ASSERT_NOT_NULL(ctx);
    memset(ctx, 0, sizeof *ctx);
    memset(&pointer, 0, sizeof pointer);
    mu_init(ctx);
    ctx->text_width = stub_text_width;
    ctx->text_height = stub_text_height;
    set_landscape();
    set_app_count(app_count);
}

/* One frame of the real bridge: touch through ui_pointer_step(), the real
 * ui_launcher_draw() in place of ui.c's ui_begin()/ui_end(). Returns the
 * chosen app, or NULL. */
static const app_t*
launcher_frame(bool down, bool pressed, bool released, int x, int y, uint32_t dt_ms) {
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

    mu_begin(ctx);
    const app_t* chosen = ui_launcher_draw(ctx, dt_ms);
    mu_end(ctx);
    pointer.over_scrollable = ctx->scroll_target != NULL;
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

/* Today's own formula (ui_launcher.c's draw_app_rows()): row i sits
 * UI_BANNER_HEIGHT + UI_ROW_GAP down, then UI_ROW_HEIGHT + UI_ROW_GAP per
 * row after that - unchanged by the migration, only how it reaches
 * microui. */
static mu_Rect
old_row_rect(int i) {
    const int y = UI_BANNER_HEIGHT + UI_ROW_GAP + i * (UI_ROW_HEIGHT + UI_ROW_GAP);
    return ui_centered_rect(ui_width(), LAUNCHER_BTN_W, UI_ROW_HEIGHT, y);
}

static void
test_rows_land_at_todays_positions_when_nothing_overflows(void) {
    fixture(3);
    idle_frames(2);

    for (int i = 0; i < 3; i++) {
        const mu_Rect r = old_row_rect(i);
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
    ctx = malloc(sizeof *ctx);
    RUN_TEST(test_rows_land_at_todays_positions_when_nothing_overflows);
    RUN_TEST(test_the_last_app_is_reachable_once_enough_apps_overflow);
    free(ctx);
    ctx = NULL;
}

SUITE_REGISTER(run_ui_launcher_suite);
