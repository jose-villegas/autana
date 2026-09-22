/*
 * The shell: boots the device, runs the frame loop, and switches between the
 * launcher and whichever app is running.
 *
 * There is exactly one frame loop on the device. It lives here, not in the
 * apps, so that switching is instant and no app can wedge the system by
 * failing to yield.
 *
 * Note this task must never return. Once firmware goes idle on this board the
 * chip stops responding to reset signalling and can only be recovered with the
 * BOOT button - see docs/notes/Flashing-and-Toolchain.md.
 */

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>

#include "app.h"
#include "boot/boot_anim.h"
#include "boot/post.h"
#include "boot/post_layout.h"
#include "boot/post_ui.h"
#include "build_id_generated.h"
#include "build_variant.h"
#include "display/display.h"
#include "display/panel_clock.h"
#include "gfx/gfx.h"
#include "gfx/gfx_font_roles.h"
#include "input/buttons.h"
#include "input/gesture.h"
#include "input/imu.h"
#include "input/imu_rotation.h"
#include "input/tilt.h"
#include "input/touch.h"
#include "ui/system_navigation.h"
#include "ui/ui.h"
#include "ui/ui_anchor.h"
#include "ui/ui_control_center.h"
#include "ui/ui_launcher.h"
#include "ui/ui_ridge.h"
#include "util/frame_cost.h"

#if CONFIG_LAUNCHER_DEVELOPMENT
#include "console/console.h"
#include "console/console_freeze.h"
#include "console/console_screenshot.h"
#include "console/console_verbs.h"
#endif

#if CONFIG_LAUNCHER_SELFTEST
#include "boot/selftest.h"
#include "console/console_runsuite.h"
#include "suites.h"
#endif

#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char* TAG = "shell";

#if CONFIG_LAUNCHER_DEVELOPMENT
#include "esp_heap_caps.h"

/* Free heap alone never predicts whether the next big allocation fits:
 * the framebuffer and an app's largest buffer each need ONE CONTIGUOUS block,
 * and
 * free space can sit outside the largest one with nothing saying where
 * it went. Printing both numbers at each boot phase says which phase
 * loses it. */
static void
heap_mark(const char* where) {
    ESP_LOGI(TAG, "HEAPMARK %-18s free %6u largest %6u", where, (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
}
#else
#define heap_mark(where) ((void)0)
#endif

#define HOME_HINT_WIDTH   120
#define HOME_HINT_HEIGHT  4
#define HOME_HINT_MARGIN  10
#define HOME_HINT_RGB     0x4A5268

/* 10 Hz: sufficient for reorientation without lag. */
#define DISPLAY_SAMPLE_MS 100

/* A stall must not reach an app as one long step. */
#define FRAME_DT_MAX_MS   250

#if CONFIG_LAUNCHER_DEVELOPMENT
#define BUILD_MARK_GLYPH        8
#define BUILD_MARK_TEXT         "D" BUILD_ID_SHORT
#define BUILD_MARK_CHARS        ((int)sizeof(BUILD_MARK_TEXT) - 1)
#define BUILD_MARK_SIZE         (BUILD_MARK_GLYPH * BUILD_MARK_CHARS)
#define BUILD_MARK_RGB          0x384054
/* The panel's rounded corners hide more than UI_MARGIN clears along an edge. */
#define BUILD_MARK_CORNER_SHIFT 32

/* Right-anchored to the upright screen's bottom-right corner, then mapped to
 * the framebuffer the way the UI's own text is. */
static void
draw_build_mark(void) {
    const int quarter = display_shell_quarter();
    const int screen_w = (quarter % 2 == 0) ? GFX_WIDTH : GFX_HEIGHT;
    const int screen_h = (quarter % 2 == 0) ? GFX_HEIGHT : GFX_WIDTH;
    const mu_Rect upright =
        ui_anchor_rect((mu_Rect){0, 0, screen_w, screen_h}, UI_ANCHOR_BOTTOM_RIGHT, UI_ANCHOR_BOTTOM_RIGHT,
                       -UI_MARGIN - BUILD_MARK_CORNER_SHIFT, -UI_MARGIN, BUILD_MARK_SIZE, BUILD_MARK_GLYPH);
    const mu_Rect box = ui_transform_rect(ui_transform_quarter_turn(quarter, GFX_WIDTH, GFX_HEIGHT), upright);

    if (gfx_region_dirty(box.x, box.y, box.w, box.h)) {
        int x = 0;
        int y = 0;
        ui_text_glyph0_origin(gfx_font_ui(), box, quarter, 1, &x, &y);
        gfx_text_turned(x, y, BUILD_MARK_TEXT, gfx_rgb(BUILD_MARK_RGB), 1, quarter);
    }
}
#endif

/* panel clock */

_Static_assert(PANEL_CLOCK_SLOW_HZ == GFX_PANEL_CLOCK_SLOW_HZ && PANEL_CLOCK_FAST_HZ == GFX_PANEL_CLOCK_FAST_HZ,
               "panel_clock.h's rates must match gfx.h's");

#define PANEL_CLOCK_NVS_NAMESPACE "shell"
#define PANEL_CLOCK_NVS_KEY       "panel_hz"

static panel_clock_t shell_panel_clock;

static bool
nvs_ready(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        err = nvs_flash_erase();
        if (err == ESP_OK) {
            err = nvs_flash_init();
        }
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS unavailable, settings are not kept: %s", esp_err_to_name(err));
    }
    return err == ESP_OK;
}

static void
load_system_panel_clock(void) {
    int32_t saved = 0;
    bool found = false;
    nvs_handle_t h;
    if (nvs_ready() && nvs_open(PANEL_CLOCK_NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        found = nvs_get_i32(h, PANEL_CLOCK_NVS_KEY, &saved) == ESP_OK;
        nvs_close(h);
    }
    panel_clock_init(&shell_panel_clock, found, saved, GFX_QSPI_HZ);
    gfx_set_panel_clock_hz(panel_clock_system_hz(&shell_panel_clock));
}

/* Whatever the app that just started or exited did to the clock or to heal,
 * the next context begins from the system value and heal's defaults. */
static void
restore_system_display_state(void) {
    gfx_set_panel_clock_hz(panel_clock_for_switch(&shell_panel_clock));
    gfx_heal_restore_defaults();
}

void
shell_set_system_panel_clock_hz(int hz) {
    if (hz == panel_clock_system_hz(&shell_panel_clock) || !panel_clock_set_system(&shell_panel_clock, hz)) {
        return;
    }
    gfx_set_panel_clock_hz(hz);
    nvs_handle_t h;
    if (!nvs_ready() || nvs_open(PANEL_CLOCK_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    if (nvs_set_i32(h, PANEL_CLOCK_NVS_KEY, hz) != ESP_OK || nvs_commit(h) != ESP_OK) {
        ESP_LOGW(TAG, "could not save the panel clock choice");
    }
    nvs_close(h);
}

int
shell_system_panel_clock_hz(void) {
    return panel_clock_system_hz(&shell_panel_clock);
}

static display_t shell_display;

int
display_shell_quarter(void) {
    return display_quarter(&shell_display);
}

/* Content-driven, not a fixed physical reference: the exit gesture lives
 * on whichever PHYSICAL edge the content's logical bottom maps to,
 * tracking rotation the same way ui_transform_rect() makes buttons/text
 * do. Not hand-derived per quarter: this table maps a strip along the
 * logical canvas's bottom edge through the same transform pipeline the
 * exhaustive sweep already proved exact. */
static gesture_edge_t
exit_edge_for_quarter(int quarter) {
    static const gesture_edge_t edge_for_quarter[4] = {
        GESTURE_EDGE_BOTTOM, /* quarter 0: Portrait */
        GESTURE_EDGE_LEFT,   /* quarter 1: Landscape */
        GESTURE_EDGE_TOP,    /* quarter 2: Portrait upside down */
        GESTURE_EDGE_RIGHT,  /* quarter 3: Landscape upside down */
    };
    return edge_for_quarter[quarter];
}

/* Control Center opens from the content's logical top, the edge opposite
 * the one that exits. */
static gesture_edge_t
opposite_edge(gesture_edge_t edge) {
    switch (edge) {
        case GESTURE_EDGE_TOP: return GESTURE_EDGE_BOTTOM;
        case GESTURE_EDGE_BOTTOM: return GESTURE_EDGE_TOP;
        case GESTURE_EDGE_LEFT: return GESTURE_EDGE_RIGHT;
        case GESTURE_EDGE_RIGHT: return GESTURE_EDGE_LEFT;
    }
    return edge;
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

/* Band mode (gfx.h) has no framebuffer for draw_home_hint() to write into,
 * and its whole band loop runs inside frame() with no chance to draw
 * afterward - see step_app(). Queuing the hint before frame() runs lets
 * whichever ui_end_for_bands() call happens this frame (fps counter, BOOT
 * menu, whichever is showing) bin it alongside its own commands. */
static void
queue_home_hint(gesture_edge_t edge) {
    int x, y, w, h;
    home_hint_rect(edge, &x, &y, &w, &h);
    ui_queue_band_overlay_rect(x, y, w, h, HOME_HINT_RGB);
}

/* Holds failing checks until touch. Prevents dead hardware diagnosis from
 * scrolling to launcher. */
static void
show_post_failures(void) {
    ESP_LOGE(TAG, "POST failed - showing report");

    /* No gravity reading has arrived this early, so the report is drawn at
     * the orientation the board is normally held at rather than at the
     * unset one. */
    const post_ui_report_t report = {
        .title = POST_LAYOUT_FAULT_TITLE,
        .footer = POST_LAYOUT_FAULT_FOOTER,
        .quarter = DISPLAY_DEFAULT_QUARTER,
        .failures_only = true,
    };
    post_ui_draw_report(&report);
    gfx_present();

    /* Long timeout for manual action, short for unattended use. */
    vTaskDelay(pdMS_TO_TICKS(8000));
}

/* What the boot animation dissolves into: the home screen as its first frame
 * will draw it, untouched and whole. */
static void
paint_launcher_under_boot(void) {
    const input_t no_input = {0};
    ui_invalidate();
    ui_launcher_frame(&no_input, 0);
}

/* True once frame() has drawn a frame for the current app that update()'s
 * caller has not yet begun presenting - see step_app(). Reset whenever the
 * running app changes, so a freshly entered one always primes first. */
static bool frame_ready;

/* The app half of gfx_request_full_redraw() (gfx.h): an app's own cache
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

static void
leave_app(const app_t** current, input_t* input, gesture_edge_t exit_edge, uint32_t dt_ms) {
    ESP_LOGI(TAG, "Leaving %s", (*current)->name);
    (*current)->exit();
    restore_system_display_state();
    *current = NULL;
    frame_ready = false;
    gfx_request_full_redraw();
    apply_pending_full_redraw(NULL);
    /* Draw it immediately, so the frame presented below is the home screen
     * rather than the app's last one. */
    ui_launcher_frame(input, dt_ms);
    /* ui_launcher_frame() just repainted its whole rect over the hint
     * strip's band, so this has to run again to put it back - dirty
     * tracking alone will not retry it, since nothing else marks that
     * band dirty on a later frame. */
    draw_home_hint(exit_edge);
}

static system_navigation_t system_navigation;
static int control_center_backdrop_quarter;

/* The backdrop is the launcher's own frame under a scrim, drawn once and
 * then painted over, so anything that replaces the framebuffer or turns the
 * content has to draw it again. */
static void
paint_control_center_backdrop(uint32_t dt_ms) {
    const input_t no_input = {0};
    ui_invalidate();
    ui_launcher_frame(&no_input, dt_ms);
    ui_control_center_dim_backdrop();
    ui_invalidate();
    control_center_backdrop_quarter = display_shell_quarter();
}

static void
step_control_center(const input_t* input, uint32_t dt_ms) {
    const bool redraw_requested = gfx_full_redraw_pending();
    gfx_full_redraw_clear_pending();
    if (redraw_requested || control_center_backdrop_quarter != display_shell_quarter()) {
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
    if (!imu_ready() || !imu_read(&sample)) {
        return;
    }
    tilt_update(&launcher_tilt, imu_gravity_screen_x(&sample), imu_gravity_screen_y(&sample), sample.az,
                imu_rotation_level(&sample), dt_ms);
    ui_ridge_set_gravity(tilt_x(&launcher_tilt), tilt_y(&launcher_tilt), tilt_strength(&launcher_tilt),
                         tilt_shake(&launcher_tilt));
}

static void
step_launcher(const app_t** current, input_t* input, gesture_edge_t exit_edge, uint32_t dt_ms) {
    if (system_navigation_step(&system_navigation, input, opposite_edge(exit_edge), exit_edge, GFX_WIDTH, GFX_HEIGHT)) {
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
    *current = chosen;
    ESP_LOGI(TAG, "Starting %s", (*current)->name);
    gfx_request_full_redraw();
    restore_system_display_state();
    (*current)->enter();
    frame_ready = false;
}

/* An app with update(): overlap it with sending the frame drawn last pass
 * (gfx_present_begin()/gfx_present_wait(), gfx.h) - skipped while priming
 * (frame_ready false), since nothing is queued yet. THIS pass's frame()
 * output is presented the same way, deferred to
 * present_unless_deferred() next pass. */
static void
step_running_app(const app_t* current, input_t* input, uint32_t dt_ms) {
    if (current->update == NULL) {
        current->frame(dt_ms, input);
        return;
    }
    if (frame_ready) {
        gfx_present_begin();
        current->update(dt_ms, input);
        gfx_present_wait();
    }
    current->frame(dt_ms, input);
    frame_ready = true;
}

static void
step_app(const app_t** current, input_t* input, uint32_t dt_ms) {
    const gesture_edge_t exit_edge = exit_edge_for_quarter(display_shell_quarter());

    if (*current == NULL) {
        step_launcher(current, input, exit_edge, dt_ms);
        return;
    }

    /* See app_t.home_gesture. Unset apps get no swipe detection or hint
     * strip. */
    if ((*current)->home_gesture && gesture_is_home_swipe(input, exit_edge, GFX_WIDTH, GFX_HEIGHT)) {
        leave_app(current, input, exit_edge, dt_ms);
        return;
    }

    /* HELD, not a plain press: an app may read a short PWR press itself for
     * its own purposes, and stealing it here would silence that everywhere
     * else in this shell too. `held` fires from the PMU's own separate
     * long-press interrupt (buttons.h), so the two are independent presses,
     * not the same edge read twice. Checked before frame() runs, so the app
     * never sees the hold that just exited it. */
    if (!(*current)->home_gesture && input->power.held) {
        leave_app(current, input, exit_edge, dt_ms);
        return;
    }

    /* Band mode's whole band loop runs inside frame(), with no chance to
     * draw anything once it returns - see queue_home_hint()'s own comment.
     * Queued before frame() runs; the trailing draw_home_hint() below
     * covers every other app unchanged. */
    if ((*current)->home_gesture && gfx_mode_current()->layout == GFX_LAYOUT_BANDS) {
        queue_home_hint(exit_edge);
    }

    apply_pending_full_redraw(*current);
    step_running_app(*current, input, dt_ms);

    if ((*current)->home_gesture && gfx_mode_current()->layout == GFX_LAYOUT_FULL_FB) {
        draw_home_hint(exit_edge);
    }
}

#if CONFIG_LAUNCHER_DEVELOPMENT
/* Report throughput on TIMER, not frames. CONFIG_LAUNCHER_DEVELOPMENT only */
static void
report_fps(int64_t now_us, int64_t* window_start, uint32_t* frames) {
    (*frames)++;

    /* Loop passes alone overstate smoothness: an idle pass sends nothing.
     * Presents that sent a strip, and the touch samples feeding them, are
     * what a swipe actually sees. */
    static uint32_t drawn;
    int full = 0, gathered = 0, partial = 0;
    gfx_get_strip_send_counts(&full, &gathered, &partial);
    if (full + gathered + partial > 0) {
        drawn++;
        gfx_reset_strip_send_counts();
    }

    const int64_t since = now_us - *window_start;
    if (since >= 1500000) {
        uint32_t points = 0, moved = 0;
        touch_take_sample_counts(&points, &moved);
        const double per_s = 1000000.0 / (double)since;
        ESP_LOGI(TAG, "%.1f fps, %.1f drawn/s, touch %.1f points/s %.1f moved/s", (double)*frames * per_s,
                 (double)drawn * per_s, (double)points * per_s, (double)moved * per_s);
        char cost[160];
        if (frame_cost_take_report(*frames, cost, sizeof cost) > 0) {
            ESP_LOGI(TAG, "ms/frame avg/worst: %s", cost);
        }
        *frames = 0;
        drawn = 0;
        *window_start = now_us;
    }
}
#endif

#if CONFIG_LAUNCHER_DEVELOPMENT
static void
park_forever(void) {
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* Two lines can never both be reached once a verb and an app's prefix, or
 * two apps' own prefixes, read the same - loud here, at boot, rather than
 * silently losing one of them to whichever an unclaimed line happens to
 * match first. */
static void
check_console_prefix_clashes(void) {
    /* At most one prefix per registered app, so the registry's own count is
     * an exact upper bound - no magic number to outgrow. */
    const char** app_prefixes = malloc(sizeof(*app_prefixes) * (size_t)app_registry_count());
    if (app_prefixes == NULL) {
        ESP_LOGE(TAG, "no memory to check console prefix clashes");
        park_forever();
    }
    int app_count = 0;
    for (const app_t* app = app_list(); app != NULL; app = app->next) {
        if (app->console != NULL) {
            app_prefixes[app_count++] = app->console->prefix;
        }
    }

    const char* from;
    const char* other;
    switch (console_find_clash(console_shared(), app_prefixes, app_count, &from, &other)) {
        case CONSOLE_CLASH_NONE: free(app_prefixes); return;
        case CONSOLE_CLASH_SPACE: ESP_LOGE(TAG, "console prefix '%s' contains a space", from); break;
        case CONSOLE_CLASH_LENGTH:
            ESP_LOGE(TAG, "console prefix '%s' plus a space does not fit CONSOLE_LINE_MAX", from);
            break;
        case CONSOLE_CLASH_VERB:
        case CONSOLE_CLASH_APP: ESP_LOGE(TAG, "console prefix '%s' clashes with '%s'", from, other); break;
    }
    park_forever();
}
#endif

/* Park rather than return on graphics failure - returning from app_main
 * leaves the chip idle and unflashable. */
static void
app_boot_init(void) {
    printf("BUILD_ID=%s\n", BUILD_ID);
    fflush(stdout);
    system_navigation_init(&system_navigation);
    tilt_reset(&launcher_tilt, IMU_COUNTS_PER_G);
    heap_mark("boot");

    /* Test SD card during panel use. */
    post_run_before_display();
    heap_mark("after sd probe");

    if (!gfx_init()) {
        ESP_LOGE(TAG, "Graphics failed to start; nothing more to do");
        while (1) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    heap_mark("after gfx_init");
    load_system_panel_clock();
#if CONFIG_LAUNCHER_DEVELOPMENT
    heap_caps_dump(MALLOC_CAP_DMA);
#endif

    if (!post_run_after_display()) {
        show_post_failures();
    }
    heap_mark("after post");

#if CONFIG_LAUNCHER_SELFTEST && CONFIG_LAUNCHER_SELFTEST_AUTORUN
    if (selftest_run() != 0) {
        ESP_LOGE(TAG, "self test reported failures");
    }
#endif

    /* The launcher has to exist, turned the way boot draws, before the
     * animation can dissolve into it. ui_init() resets the transform to
     * identity, so DISPLAY_DEFAULT_QUARTER is applied here or the board
     * would start upright and visibly turn into place. */
#if CONFIG_LAUNCHER_DEVELOPMENT
    check_console_prefix_clashes();
#endif
    display_init(&shell_display);
    shell_display.quarter = DISPLAY_DEFAULT_QUARTER;
    ui_launcher_init();
    ui_set_transform(ui_transform_quarter_turn(display_quarter(&shell_display), GFX_WIDTH, GFX_HEIGHT));

    boot_anim_set_ending_backdrop(paint_launcher_under_boot);
    boot_anim_run();
    gfx_request_full_redraw();
    heap_mark("after boot anim");

    touch_start();
    buttons_start();
#if CONFIG_LAUNCHER_DEVELOPMENT
    console_start();
#endif

    if (!imu_init()) {
        ESP_LOGW(TAG, "No IMU - display orientation stays upright");
    }
    heap_mark("shell ready");
}

#if CONFIG_LAUNCHER_SELFTEST
/* See console/console.c for framebuffer contention explanation. */
static void
run_pending_selftest_suite(void) {
    char runsuite_name[64];
    if (!console_runsuite_take_request(runsuite_name, sizeof runsuite_name)) {
        return;
    }
    const bool found = suites_run_one(runsuite_name);
    if (!found) {
        ESP_LOGE(TAG, "no suite named '%s' is registered", runsuite_name);
    }
    /* On its own line, so a harness knows the suite ended without having to
     * guess from how long the console has been quiet. */
    printf("\nRUNSUITE_COMPLETE name=%s found=%d\n", runsuite_name, found ? 1 : 0);
    fflush(stdout);
    /* A suite draws, clears and presents on its own, outside the shell's
     * own dirty tracking - the next real frame must repaint in full rather
     * than trust whatever a test left behind. */
    gfx_request_full_redraw();
}
#endif

static void
sample_display_orientation(int64_t now_us, int64_t* next_sample_us) {
    if (now_us < *next_sample_us) {
        return;
    }
    *next_sample_us = now_us + (int64_t)DISPLAY_SAMPLE_MS * 1000;

    imu_sample_t sample;
    if (!imu_ready() || !imu_read(&sample)) {
        return;
    }
    const int gx = imu_gravity_screen_x(&sample);
    const int gy = imu_gravity_screen_y(&sample);
    if (display_update(&shell_display, gx, gy)) {
        ui_set_transform(ui_transform_quarter_turn(display_quarter(&shell_display), GFX_WIDTH, GFX_HEIGHT));
        gfx_request_full_redraw();
    }
}

#if CONFIG_LAUNCHER_DEVELOPMENT
/* `<PREFIX>_ERR <reason>` - the app's own prefix in capitals, like a
 * verb's own TUNE_ERR, so a caller matching `<PREFIX>_ERR`
 * (docs/tools/Autana-CLI.md) recognises either failure below the same way
 * it recognises a completed reply. */
static void
reply_app_err(const char* prefix, const char* reason) {
    char upper[CONSOLE_LINE_MAX];
    size_t i = 0;
    for (; prefix[i] != '\0' && i < sizeof(upper) - 1; i++) {
        upper[i] = (char)toupper((unsigned char)prefix[i]);
    }
    upper[i] = '\0';
    printf("%s_ERR %s\n", upper, reason);
    fflush(stdout);
}

/* Takes at most one line no verb claimed and routes it by prefix: to
 * `current` if its own prefix matches (a reply_app_err() "not handled" if
 * its handler then declines `args`), a reply_app_err() "not running" if
 * some other app's prefix matches, logged otherwise. */
static void
offer_console_line(const app_t* current) {
    char line[CONSOLE_LINE_MAX];
    if (!console_take_unclaimed_line(line, sizeof line)) {
        return;
    }

    for (const app_t* app = app_list(); app != NULL; app = app->next) {
        if (app->console == NULL) {
            continue;
        }
        const char* args;
        if (!console_word_match(line, app->console->prefix, &args)) {
            continue;
        }
        if (app != current) {
            reply_app_err(app->console->prefix, "not running");
            return;
        }
        if (!app->console->handle(args)) {
            reply_app_err(app->console->prefix, "not handled");
        }
        return;
    }

    ESP_LOGI(TAG, "ignoring line: '%s'", line);
}

static void
run_dev_frame_extras(input_t* input, const app_t* current) {
    if (gfx_mode_current()->layout == GFX_LAYOUT_FULL_FB) {
        draw_build_mark();
    }
    if (console_screenshot_take_request()) {
        console_screenshot_dump(input, current);
        gfx_request_full_redraw();
    }
    offer_console_line(current);
}
#endif

/* An app with update() manages its own present begin/wait inside step_app(),
 * deferring the frame just drawn to next pass's begin - see its own
 * comment. Everything else (the launcher included) keeps presenting here,
 * synchronously, exactly as before. */
static void
present_unless_deferred(const app_t* current) {
    if (current == NULL || current->update == NULL) {
        FRAME_COST_BEGIN(began);
        gfx_present();
        FRAME_COST_END(began, "present");
    }
}

static void
app_main_loop(void) {
    const app_t* current = NULL; /* NULL means the launcher is showing */
    input_t input = {0};
    int64_t previous_us = esp_timer_get_time();
#if CONFIG_LAUNCHER_DEVELOPMENT
    int64_t fps_window_start = previous_us;
    uint32_t frames = 0;
#endif
    int64_t next_display_sample_us = previous_us;

    const int app_count = app_registry_count();
    ESP_LOGI(TAG, "Ready, %d app%s registered", app_count, app_count == 1 ? "" : "s");

    while (1) {
        const int64_t now_us = esp_timer_get_time();
        uint32_t dt_ms = (uint32_t)((now_us - previous_us) / 1000);
        previous_us = now_us;
        if (dt_ms > FRAME_DT_MAX_MS) {
            dt_ms = FRAME_DT_MAX_MS;
        }
        FRAME_COST_BEGIN(rest_began);

#if CONFIG_LAUNCHER_SELFTEST
        run_pending_selftest_suite();
#endif

        touch_read(&input);
        buttons_read(&input.boot, &input.power);
        sample_display_orientation(now_us, &next_display_sample_us);

#if CONFIG_LAUNCHER_DEVELOPMENT
        /* After the reads above on purpose: a held device still answers
         * the console and still latches an orientation change's own full
         * redraw, so the STEP that follows a rotation draws the frame that
         * rotation asked for. */
        if (!console_freeze_frame_allowed()) {
            /* Held frames never reach run_dev_frame_extras() below, so a
             * frozen board still answers a command here - the obvious use
             * is freeze, inspect, step. */
            offer_console_line(current);
            FRAME_COST_END(rest_began, "frame.rest");
            vTaskDelay(1);
            continue;
        }
#endif

        step_app(&current, &input, dt_ms);

#if CONFIG_LAUNCHER_DEVELOPMENT
        run_dev_frame_extras(&input, current);
#endif

        present_unless_deferred(current);
#if CONFIG_LAUNCHER_DEVELOPMENT
        report_fps(now_us, &fps_window_start, &frames);
#endif
        FRAME_COST_END(rest_began, "frame.rest");

        /* Yield so the idle task can feed the watchdog. */
        vTaskDelay(1);
    }
}

void
app_main(void) {
    app_boot_init();
    app_main_loop();
}
