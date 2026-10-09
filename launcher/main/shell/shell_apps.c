/*
 * The shell's app switching: what it does on entering and leaving an app,
 * the launcher and Control Center it steps while none runs, the home hint,
 * and when a frame is presented.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app/app.h"
#include "app/app_arena.h"
#include "display/display.h"
#include "gfx/draw/gfx_draw.h"
#include "gfx/gfx.h"
#include "gfx/present/gfx_mode.h"
#include "gfx/present/gfx_present.h"
#include "input/gesture.h"
#include "input/imu.h"
#include "input/imu_rotation.h"
#include "input/input.h"
#include "input/tilt.h"
#include "shell/shell.h"
#include "shell/shell_apps.h"
#include "shell/shell_frame.h"
#include "shell/shell_system.h"
#include "ui/system_navigation.h"
#include "ui/ui.h"
#include "ui/ui_anchor.h"
#include "ui/ui_control_center.h"
#include "ui/ui_launcher.h"
#include "ui/ui_ridge.h"
#include "util/runtime/frame_cost.h"
#include "util/runtime/frame_watch.h"
#include "util/runtime/memory.h"

#include "esp_log.h"

static const char* TAG = "shell";

#if CONFIG_LAUNCHER_DEVELOPMENT
static size_t app_internal_free_before_enter;
static size_t app_8bit_free_before_enter;
#endif

#define HOME_HINT_WIDTH  120
#define HOME_HINT_HEIGHT 4
#define HOME_HINT_MARGIN 10
#define HOME_HINT_RGB    0x4A5268

/* Content-driven, not a fixed physical reference: the exit gesture lives
 * on whichever PHYSICAL edge the content's logical bottom maps to,
 * tracking rotation the same way ui_transform_rect() makes buttons/text
 * do. Not hand-derived per quarter: this table maps a strip along the
 * logical canvas's bottom edge through the same transform pipeline the
 * exhaustive sweep already proved exact. */
gesture_edge_t
shell_exit_edge_for_quarter(int quarter) {
    static const gesture_edge_t edge_for_quarter[4] = {
        GESTURE_EDGE_BOTTOM, /* quarter 0: Portrait */
        GESTURE_EDGE_LEFT,   /* quarter 1: Landscape */
        GESTURE_EDGE_TOP,    /* quarter 2: Portrait upside down */
        GESTURE_EDGE_RIGHT,  /* quarter 3: Landscape upside down */
    };
    return edge_for_quarter[quarter];
}

/* chrome */

static void
home_hint_rect(gesture_edge_t edge, int* x, int* y, int* w, int* h) {
    ui_anchor_t anchor = UI_ANCHOR_TOP_LEFT;
    int offset_x = 0;
    int offset_y = 0;
    int width = 0;
    int height = 0;

    switch (edge) {
        case GESTURE_EDGE_TOP:
            anchor = UI_ANCHOR_TOP;
            offset_x = 0;
            offset_y = HOME_HINT_MARGIN;
            width = HOME_HINT_WIDTH;
            height = HOME_HINT_HEIGHT;
            break;
        case GESTURE_EDGE_BOTTOM:
            anchor = UI_ANCHOR_BOTTOM;
            offset_x = 0;
            offset_y = -HOME_HINT_MARGIN;
            width = HOME_HINT_WIDTH;
            height = HOME_HINT_HEIGHT;
            break;
        case GESTURE_EDGE_LEFT:
            anchor = UI_ANCHOR_LEFT;
            offset_x = HOME_HINT_MARGIN;
            offset_y = 0;
            width = HOME_HINT_HEIGHT;
            height = HOME_HINT_WIDTH;
            break;
        case GESTURE_EDGE_RIGHT:
            anchor = UI_ANCHOR_RIGHT;
            offset_x = -HOME_HINT_MARGIN;
            offset_y = 0;
            width = HOME_HINT_HEIGHT;
            height = HOME_HINT_WIDTH;
            break;
    }

    const mu_Rect rect =
        ui_anchor_rect((mu_Rect){0, 0, GFX_WIDTH, GFX_HEIGHT}, anchor, anchor, offset_x, offset_y, width, height);
    *x = rect.x;
    *y = rect.y;
    *w = rect.w;
    *h = rect.h;
}

static void
draw_home_hint(gesture_edge_t edge) {
    int x, y, w, h;
    home_hint_rect(edge, &x, &y, &w, &h);

    if (!gfx_region_dirty(x, y, w, h)) {
        return;
    }

    gfx_fill_rect(x, y, w, h, gfx_rgb(HOME_HINT_RGB));
}

/* Transient and expanded pictures need the hint in their overlay command
 * list before frame() bins it. */
static void
queue_home_hint(gesture_edge_t edge) {
    int x, y, w, h;
    home_hint_rect(edge, &x, &y, &w, &h);
    ui_queue_band_overlay_rect(x, y, w, h, HOME_HINT_RGB);
}

/* What the boot animation dissolves into: the home screen as its first frame
 * will draw it, untouched and whole. */
void
shell_paint_home_under_boot(void) {
    ui_invalidate();
    ui_launcher_frame(NULL, 0);
}

/* True once frame() has drawn a frame for the current app that update()'s
 * caller has not yet begun presenting; see step_running_app(). Reset whenever the
 * running app changes, so a freshly entered one always primes first. */
static bool frame_ready;
static bool exit_requested;

void
shell_request_exit(void) {
    exit_requested = true;
}

static bool
app_band_active(const app_t* app) {
    const gfx_mode_t* mode = gfx_mode_current();
    return app->draw_band != NULL && mode->layout == GFX_LAYOUT_BANDS;
}

/* The app half of gfx_request_full_redraw() (gfx_present.h): an app's own cache
 * beyond the framebuffer, if it keeps one, or the launcher's ui.c canvas
 * cache while none is running. Consumes the pending flag before whichever
 * of the two draws next, not after: a request made inside that very
 * frame() call (an app invalidating itself) must reach the FOLLOWING
 * pass's check, not be cleared out from under it before ever being read. */
static void
apply_pending_full_redraw(const app_t* app) {
    if (!gfx_full_redraw_pending()) {
        return;
    }
    gfx_full_redraw_clear_pending();
    if (app != NULL) {
        if (app->invalidate != NULL) {
            app->invalidate();
        }
    } else {
        ui_invalidate();
    }
}

static system_navigation_t system_navigation;
static int control_center_backdrop_quarter;

void
shell_exit_app(const app_t** current) {
    ESP_LOGI(TAG, "Leaving %s", (*current)->name);
    (*current)->exit();
    shell_systems_app_exit();
#if CONFIG_LAUNCHER_DEVELOPMENT
    const size_t internal_after_exit = memory_free_bytes(MEMORY_INTERNAL);
    const size_t eight_bit_after_exit = memory_free_bytes(MEMORY_8BIT);
    if (internal_after_exit < app_internal_free_before_enter) {
        ESP_LOGW(TAG, "App %s kept %u internal heap bytes (other tasks can move this)", (*current)->name,
                 (unsigned)(app_internal_free_before_enter - internal_after_exit));
    }
    if (eight_bit_after_exit < app_8bit_free_before_enter) {
        ESP_LOGW(TAG, "App %s kept %u 8-bit heap bytes (other tasks can move this)", (*current)->name,
                 (unsigned)(app_8bit_free_before_enter - eight_bit_after_exit));
    }
#endif
    app_arena_rewind(0);
    display_restore_system_state();
    ui_invalidate();
    frame_watch_restart();
    *current = NULL;
    frame_ready = false;
    gfx_request_full_redraw();
}

void
shell_leave_app(const app_t** current, input_t* input, gesture_edge_t exit_edge, uint32_t dt_ms) {
    shell_exit_app(current);
    apply_pending_full_redraw(NULL);
    /* Draw it immediately, so the frame presented below is the home screen
     * rather than the app's last one. */
    ui_launcher_frame(input, dt_ms);
    /* ui_launcher_frame() just repainted its whole rect over the hint
     * strip's band, so this has to run again to put it back; dirty
     * tracking alone will not retry it, since nothing else marks that
     * band dirty on a later frame. */
    draw_home_hint(exit_edge);
}

void
shell_start_app(const app_t** current, const app_t* next) {
    *current = next;
    ESP_LOGI(TAG, "Starting %s", (*current)->name);
    system_navigation_init(&system_navigation);
    gfx_request_full_redraw();
    display_restore_system_state();
    exit_requested = false;
#if CONFIG_LAUNCHER_DEVELOPMENT
    app_internal_free_before_enter = memory_free_bytes(MEMORY_INTERNAL);
    app_8bit_free_before_enter = memory_free_bytes(MEMORY_8BIT);
#endif
    (*current)->enter();
    frame_ready = false;
    frame_watch_restart();
}

/* The backdrop is the launcher's own frame under a scrim, drawn once and
 * then painted over, so anything that replaces the framebuffer or turns the
 * content has to draw it again. */
static void
paint_control_center_backdrop(uint32_t dt_ms) {
    ui_invalidate();
    ui_launcher_frame(NULL, dt_ms);
    ui_control_center_dim_backdrop();
    ui_invalidate();
    control_center_backdrop_quarter = display_quarter_now();
}

static void
step_control_center(const input_t* input, uint32_t dt_ms) {
    const bool redraw_requested = gfx_full_redraw_pending();
    gfx_full_redraw_clear_pending();
    if (redraw_requested || control_center_backdrop_quarter != display_quarter_now()) {
        paint_control_center_backdrop(dt_ms);
    }
    ui_control_center_frame(input);
}

static tilt_t launcher_tilt;

/* The launcher's backdrop keeps level with the horizon, so it needs gravity
 * every frame, not at the orientation logic's own slower pace. */
static void
feed_launcher_gravity(uint32_t dt_ms) {
    imu_sample_t sample;
    if (!imu_read(&sample)) {
        return;
    }
    const vec2i_t gravity = imu_gravity_screen(&sample);
    tilt_update(&launcher_tilt, gravity.x, gravity.y, sample.az, imu_rotation_level(&sample), dt_ms);
    ui_ridge_set_gravity(tilt_x(&launcher_tilt), tilt_y(&launcher_tilt), tilt_strength(&launcher_tilt),
                         tilt_shake(&launcher_tilt));
}

static void
step_launcher(const app_t** current, input_t* input, gesture_edge_t exit_edge, uint32_t dt_ms) {
    if (system_navigation_step(&system_navigation, input, gesture_opposite_edge(exit_edge), exit_edge, GFX_WIDTH,
                               GFX_HEIGHT)) {
        gfx_request_full_redraw();
    }
    if (system_navigation.screen == SYSTEM_SCREEN_CONTROL_CENTER) {
        step_control_center(input, dt_ms);
        return;
    }
    apply_pending_full_redraw(NULL);
    feed_launcher_gravity(dt_ms);
    const app_t* chosen = ui_launcher_frame(input, dt_ms);
    if (chosen == NULL) {
        draw_home_hint(exit_edge);
        return;
    }
    shell_start_app(current, chosen);
}

/* Whether the app's frame is presented a pass late, so that update() and the
 * systems' update phase run while the last one is still being sent. */
static bool
overlaps_present(const app_t* current) {
    return current->update != NULL || shell_systems_overlap_present();
}

/* An overlapping pass sends last pass's frame (gfx_present_begin() and
 * gfx_present_wait()) while this one updates; nothing is sent while priming
 * (frame_ready false). This pass's frame() is sent next pass. gfx_band_run()
 * does nothing outside GFX_LAYOUT_BANDS. */
static void
step_running_app(const app_t* current, input_t* input, uint32_t dt_ms) {
    const bool overlap = overlaps_present(current);
    if (overlap && frame_ready) {
        gfx_present_begin();
        if (current->update != NULL) {
            current->update(dt_ms, input);
        }
        shell_systems_update(dt_ms);
        FRAME_COST_BEGIN(waited);
        gfx_present_wait();
        FRAME_COST_END(waited, "present.wait");
    }
    ui_clear_band_overlay();
    if (overlap) {
        shell_systems_compose(dt_ms);
    }
    if (current->home_gesture && (app_band_active(current) || gfx_frame_expanded())) {
        queue_home_hint(shell_exit_edge_for_quarter(display_quarter_now()));
    }
    current->frame(dt_ms, input);
    gfx_band_run(current->draw_band, ui_replay_band);
    if (overlap) {
        frame_ready = true;
    }
}

void
shell_step_app(const app_t** current, input_t* input, uint32_t dt_ms) {
    const gesture_edge_t exit_edge = shell_exit_edge_for_quarter(display_quarter_now());

    if (*current == NULL) {
        step_launcher(current, input, exit_edge, dt_ms);
        return;
    }

    if (exit_requested) {
        exit_requested = false;
        shell_leave_app(current, input, exit_edge, dt_ms);
        return;
    }

    /* See app_t.home_gesture. Unset apps get no swipe detection or hint
     * strip. */
    if ((*current)->home_gesture && gesture_is_home_swipe(input, exit_edge, GFX_WIDTH, GFX_HEIGHT)) {
        shell_leave_app(current, input, exit_edge, dt_ms);
        return;
    }

    /* HELD, not a plain press: an app may read a short PWR press itself for
     * its own purposes, and stealing it here would silence that everywhere
     * else in this shell too. `held` fires from the PMU's own separate
     * long-press interrupt (buttons.h), so the two are independent presses,
     * not the same edge read twice. Checked before frame() runs, so the app
     * never sees the hold that just exited it. */
    if (!(*current)->home_gesture && input->power.held) {
        shell_leave_app(current, input, exit_edge, dt_ms);
        return;
    }

    apply_pending_full_redraw(*current);
    step_running_app(*current, input, dt_ms);

    if ((*current)->home_gesture && gfx_mode_current()->layout == GFX_LAYOUT_FULL_FB && !gfx_frame_expanded()) {
        draw_home_hint(exit_edge);
    }
}

void
shell_apps_init(void) {
    gfx_set_frame_overlay(shell_frame_overlay);
    system_navigation_init(&system_navigation);
    tilt_reset(&launcher_tilt, IMU_COUNTS_PER_G);
}

#if CONFIG_LAUNCHER_SELFTEST
static int shell_test_enters;
static int shell_test_frames;
static int shell_test_exits;
static int shell_test_updates;
static int shell_test_band_draws;

#define SHELL_TEST_ARENA_TAKE 1024u

static size_t shell_test_arena_at_enter;
static size_t shell_test_arena_at_exit;

static void
shell_test_enter(void) {
    shell_test_enters++;
    shell_test_arena_at_enter = app_arena_mark();
    (void)app_arena_take(SHELL_TEST_ARENA_TAKE, 1);
}

static void
shell_test_frame(uint32_t dt_ms, const input_t* input) {
    shell_test_frames++;
}

static void
shell_test_exit(void) {
    shell_test_exits++;
    shell_test_arena_at_exit = app_arena_mark();
}

static const app_t shell_test_app = {
    .name = "Shell exit test",
    .enter = shell_test_enter,
    .frame = shell_test_frame,
    .exit = shell_test_exit,
};

static void
shell_test_band_enter(void) {
    const gfx_mode_request_t request = {.layout = GFX_LAYOUT_BANDS};
    (void)gfx_mode_enter(&request);
}

static void
shell_test_band_update(uint32_t dt_ms, const input_t* input) {
    (void)dt_ms;
    (void)input;
    shell_test_updates++;
}

static void
shell_test_band_frame(uint32_t dt_ms, const input_t* input) {
    shell_test_frame(dt_ms, input);
    gfx_mark_dirty(0, 0, GFX_WIDTH, GFX_HEIGHT);
}

static void
shell_test_draw_band(int row0, int row1, gfx_color_t* target) {
    (void)row0;
    shell_test_band_draws++;
    for (int i = 0; i < GFX_WIDTH * (row1 - row0); i++) {
        target[i] = 0;
    }
}

static void
shell_test_band_exit(void) {
    gfx_mode_exit();
}

static const app_t shell_test_band_app = {
    .name = "Shell band test",
    .enter = shell_test_band_enter,
    .frame = shell_test_band_frame,
    .draw_band = shell_test_draw_band,
    .update = shell_test_band_update,
    .exit = shell_test_band_exit,
};

void
shell_test_fixture(void) {
    shell_test_enters = 0;
    shell_test_frames = 0;
    shell_test_exits = 0;
    shell_test_updates = 0;
    shell_test_band_draws = 0;
    exit_requested = false;
}

bool
shell_test_band_update_frame_and_present(void) {
    const app_t* current = NULL;
    input_t input = {0};
    shell_start_app(&current, &shell_test_band_app);
    shell_step_app(&current, &input, 16);
    shell_present_unless_deferred(current);

    frame_watch_test_begin();
    for (int i = 0; i <= FRAME_WATCH_WARMUP; i++) {
        shell_step_app(&current, &input, 16);
        shell_present_unless_deferred(current);
    }
    const frame_watch_verdict_t verdict = frame_watch_test_end();

    const int passes = FRAME_WATCH_WARMUP + 1;
    const gfx_mode_t* const mode = gfx_mode_current();
    const bool stepped = mode->layout == GFX_LAYOUT_BANDS && shell_test_updates == passes
                         && shell_test_frames == passes + 1
                         && shell_test_band_draws == (passes + 1) * (mode->height / mode->band_height);
    if (current != NULL) {
        shell_exit_app(&current);
    }
    return stepped && verdict.frames == 1;
}

bool
shell_test_requested_exit(void) {
    const app_t* current = NULL;
    input_t input = {0};
    shell_start_app(&current, &shell_test_app);
    shell_step_app(&current, &input, 16);
    const bool ordinary =
        current == &shell_test_app && shell_test_enters == 1 && shell_test_frames == 1 && shell_test_exits == 0;

    shell_request_exit();
    shell_step_app(&current, &input, 16);
    const bool left = current == NULL && shell_test_frames == 1 && shell_test_exits == 1;
    if (current == NULL) {
        shell_step_app(&current, &input, 16);
    }
    const bool launcher_next = current == NULL && shell_test_frames == 1 && shell_test_exits == 1;
    if (current != NULL) {
        shell_exit_app(&current);
    }
    exit_requested = false;
    return ordinary && left && launcher_next;
}

bool
shell_test_leaving_empties_the_arena_after_exit(void) {
    const app_t* current = NULL;
    shell_start_app(&current, &shell_test_app);
    shell_exit_app(&current);
    const bool kept_through_exit = shell_test_arena_at_exit == SHELL_TEST_ARENA_TAKE;
    const bool emptied = app_arena_mark() == 0;
    shell_start_app(&current, &shell_test_app);
    const bool next_visit_empty = shell_test_arena_at_enter == 0 && shell_test_enters == 2;
    shell_exit_app(&current);
    exit_requested = false;
    return kept_through_exit && emptied && next_visit_empty;
}

bool
shell_test_stale_exit_is_cleared(void) {
    const app_t* current = NULL;
    input_t input = {0};
    shell_request_exit();
    shell_start_app(&current, &shell_test_app);
    shell_step_app(&current, &input, 16);
    const bool first_frame =
        current == &shell_test_app && shell_test_enters == 1 && shell_test_frames == 1 && shell_test_exits == 0;
    if (current != NULL) {
        shell_exit_app(&current);
    }
    exit_requested = false;
    return first_frame;
}

static int shell_test_system_updates;
static int shell_test_system_composes;
static int shell_test_system_exits;
static int shell_test_app_exits_at_system_exit;
static bool shell_test_system_overlaps;

static void
shell_test_system_update(uint32_t dt_ms) {
    (void)dt_ms;
    shell_test_system_updates++;
}

static void
shell_test_system_compose(uint32_t dt_ms) {
    (void)dt_ms;
    shell_test_system_composes++;
}

static void
shell_test_system_app_exit(void) {
    shell_test_system_exits++;
    shell_test_app_exits_at_system_exit = shell_test_exits;
}

static bool
shell_test_system_overlap(void) {
    return shell_test_system_overlaps;
}

static shell_system_t shell_test_system;

/* The registered systems stood aside for one test system, so what the test
 * sees depends on the shell alone; returns the real list to put back. */
static shell_system_t*
shell_test_swap_in_system(void) {
    shell_test_system = (shell_system_t){
        .name = "shell test",
        .order = SHELL_ORDER_SCENE,
        .update = shell_test_system_update,
        .compose = shell_test_system_compose,
        .app_exit = shell_test_system_app_exit,
        .overlaps_present = shell_test_system_overlap,
    };
    shell_system_t* const real = shell_system_swap_for_test(NULL);
    shell_system_register(&shell_test_system);
    return real;
}

bool
shell_test_a_system_asking_for_overlap_takes_the_overlap_path(void) {
    /* Three passes of an app with no update(): the first only primes. */
    static const struct {
        bool overlaps;
        int updates;
        int composes;
    } cases[] = {
        {true, 2, 3},
        {false, 0, 0},
    };

    shell_system_t* const real = shell_test_swap_in_system();
    bool as_asked = true;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        const app_t* current = NULL;
        input_t input = {0};
        shell_test_system_overlaps = cases[i].overlaps;
        shell_test_system_updates = 0;
        shell_test_system_composes = 0;
        shell_start_app(&current, &shell_test_app);
        for (int pass = 0; pass < 3; pass++) {
            shell_step_app(&current, &input, 16);
            shell_present_unless_deferred(current);
        }
        as_asked = as_asked && shell_test_system_updates == cases[i].updates
                   && shell_test_system_composes == cases[i].composes;
        if (current != NULL) {
            shell_exit_app(&current);
        }
    }
    shell_system_swap_for_test(real);
    exit_requested = false;
    return as_asked;
}

bool
shell_test_leaving_runs_the_systems_app_exit_after_the_apps_exit(void) {
    shell_system_t* const real = shell_test_swap_in_system();
    shell_test_system_exits = 0;
    shell_test_app_exits_at_system_exit = -1;
    const app_t* current = NULL;
    shell_start_app(&current, &shell_test_app);
    const bool none_while_running = shell_test_system_exits == 0;
    shell_exit_app(&current);
    const bool once_after_the_app =
        shell_test_system_exits == 1 && shell_test_exits == 1 && shell_test_app_exits_at_system_exit == 1;
    shell_system_swap_for_test(real);
    exit_requested = false;
    return none_while_running && once_after_the_app;
}
#endif

void
shell_present_unless_deferred(const app_t* current) {
    if (current == NULL || !overlaps_present(current)) {
        FRAME_COST_BEGIN(began);
        gfx_present();
        FRAME_COST_END(began, "present");
    }
}
