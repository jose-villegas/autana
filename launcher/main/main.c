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

#include <stdint.h>
#include <string.h>

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
#include "input/touch.h"
#include "ui/ui.h"
#include "ui/ui_anchor.h"
#include "ui/ui_launcher.h"

#if CONFIG_LAUNCHER_DEVELOPMENT
#include "util/screenshot.h"
#endif

#if CONFIG_LAUNCHER_SELFTEST
#include "boot/selftest.h"
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

/* --- panel clock --------------------------------------------------------- */

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

/* --- app registry ------------------------------------------------------- */

/* Filled in before app_main() by the constructors APP_REGISTER() emits. No
 * app is named here; see app.h for why. */
static const app_t* apps[APP_MAX];
static int apps_registered;

void
app_register(const app_t* app) {
    if (apps_registered >= APP_MAX) {
        ESP_LOGE(TAG, "More than %d apps registered; '%s' was dropped", APP_MAX, app->name);
        return;
    }
    apps[apps_registered++] = app;
}

const app_t* const*
app_list(void) {
    return apps;
}

int
app_list_count(void) {
    return apps_registered;
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

static void
sort_apps(void) {
    for (int i = 1; i < apps_registered; i++) {
        const app_t* const key = apps[i];
        int j = i - 1;
        while (j >= 0 && strcmp(apps[j]->name, key->name) > 0) {
            apps[j + 1] = apps[j];
            j--;
        }
        apps[j + 1] = key;
    }
}

/* --- chrome ------------------------------------------------------------- */

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

/* --- main --------------------------------------------------------------- */

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

static void
step_launcher(const app_t** current, input_t* input, gesture_edge_t exit_edge, uint32_t dt_ms) {
    apply_pending_full_redraw(NULL);
    const int chosen = ui_launcher_frame(input, dt_ms);
    if (chosen < 0 || chosen >= apps_registered) {
        draw_home_hint(exit_edge);
        return;
    }
    *current = apps[chosen];
    ESP_LOGI(TAG, "Starting %s", (*current)->name);
    gfx_request_full_redraw();
    restore_system_display_state();
    (*current)->enter();
    frame_ready = false;
}

/* An app with update(): overlap it with sending the frame drawn last pass
 * (gfx_present_begin()/_wait(), gfx.h) - skipped while priming (frame_ready
 * false), since nothing is queued yet. THIS pass's frame() output is
 * presented the same way, deferred to present_unless_deferred() next
 * pass. */
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
        *frames = 0;
        drawn = 0;
        *window_start = now_us;
    }
}
#endif

/* Park rather than return on graphics failure - returning from app_main
 * leaves the chip idle and unflashable. */
static void
app_boot_init(void) {
    printf("BUILD_ID=%s\n", BUILD_ID);
    fflush(stdout);
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

    boot_anim_run();
    heap_mark("after boot anim");

    touch_start();
    buttons_start();
#if CONFIG_LAUNCHER_DEVELOPMENT
    screenshot_start();
#endif

    if (!imu_init()) {
        ESP_LOGW(TAG, "No IMU - display orientation stays upright");
    }
    display_init(&shell_display);

    shell_display.quarter = DISPLAY_DEFAULT_QUARTER;

    ui_launcher_init();
    heap_mark("shell ready");

    /* ui_init() above already reset the transform to identity (it has to,
     * so a stale one from a previous host test can never leak in), so
     * DISPLAY_DEFAULT_QUARTER is not actually in force yet - apply it
     * once here before the first frame is built, or the board would
     * start upright and visibly turn into place. */
    ui_set_transform(ui_transform_quarter_turn(display_quarter(&shell_display), GFX_WIDTH, GFX_HEIGHT));
}

#if CONFIG_LAUNCHER_SELFTEST
/* See util/screenshot.c for framebuffer contention explanation. */
static void
run_pending_selftest_suite(void) {
    char runsuite_name[64];
    if (!screenshot_take_runsuite_request(runsuite_name, sizeof runsuite_name)) {
        return;
    }
    if (!suites_run_one(runsuite_name)) {
        ESP_LOGE(TAG, "no suite named '%s' is registered", runsuite_name);
    }
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
static void
run_dev_frame_extras(input_t* input, const app_t* current) {
    if (gfx_mode_current()->layout == GFX_LAYOUT_FULL_FB) {
        draw_build_mark();
    }
    if (screenshot_take_request()) {
        screenshot_dump(input, current);
        gfx_request_full_redraw();
    }
}
#endif

/* An app with update() manages its own present begin/wait inside step_app(),
 * deferring the frame just drawn to next pass's begin - see its own
 * comment. Everything else (the launcher included) keeps presenting here,
 * synchronously, exactly as before. */
static void
present_unless_deferred(const app_t* current) {
    if (current == NULL || current->update == NULL) {
        gfx_present();
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

    sort_apps();
    ESP_LOGI(TAG, "Ready, %d app%s registered", apps_registered, apps_registered == 1 ? "" : "s");

    while (1) {
        const int64_t now_us = esp_timer_get_time();
        uint32_t dt_ms = (uint32_t)((now_us - previous_us) / 1000);
        previous_us = now_us;
        if (dt_ms > 250) {
            dt_ms = 250; /* clamp, so a stall does not jump animation */
        }

#if CONFIG_LAUNCHER_SELFTEST
        run_pending_selftest_suite();
#endif

        touch_read(&input);
        buttons_read(&input.boot, &input.power);
        sample_display_orientation(now_us, &next_display_sample_us);

        step_app(&current, &input, dt_ms);

#if CONFIG_LAUNCHER_DEVELOPMENT
        run_dev_frame_extras(&input, current);
#endif

        present_unless_deferred(current);
#if CONFIG_LAUNCHER_DEVELOPMENT
        report_fps(now_us, &fps_window_start, &frames);
#endif

        /* Yield so the idle task can feed the watchdog. */
        vTaskDelay(1);
    }
}

void
app_main(void) {
    app_boot_init();
    app_main_loop();
}
