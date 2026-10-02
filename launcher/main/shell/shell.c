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
 * BOOT button; see docs/notes/Flashing-and-Toolchain.md.
 */

#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "app.h"
#include "display/display.h"
#include "display/display_shell.h"
#include "gfx/gfx.h"
#include "gfx/gfx_font_roles.h"
#include "input/gesture.h"
#include "input/input.h"
#include "input/input_shell.h"
#include "input/touch.h"
#include "shell/shell.h"
#include "shell/shell_apps.h"
#include "shell/shell_system.h"
#include "ui/ui.h"
#include "ui/ui_anchor.h"
#include "util/build_id.h"
#include "util/frame_cost.h"
#include "util/frame_watch.h"
#include "util/timing.h"

#if CONFIG_LAUNCHER_DEVELOPMENT
#include "console/console.h"
#include "console/console_frame_watch.h"
#include "console/console_freeze.h"
#include "console/console_navigation.h"
#include "console/console_navigation_parse.h"
#include "console/console_screenshot.h"
#include "console/console_verbs.h"
#endif

#if CONFIG_LAUNCHER_SELFTEST
#include "console/console_runsuite.h"
#include "suites.h"
#endif

#include "esp_log.h"

static const char* TAG = "shell";

/* A stall must not reach an app as one long step. */
#define FRAME_DT_MAX_MS 250

#if CONFIG_LAUNCHER_DEVELOPMENT
#define BUILD_MARK_GLYPH        8
#define BUILD_MARK_CHARS        (1 + BUILD_ID_SHORT_CHARS)
#define BUILD_MARK_SIZE         (BUILD_MARK_GLYPH * BUILD_MARK_CHARS)
#define BUILD_MARK_RGB          0x384054
/* The panel's rounded corners hide more than UI_MARGIN clears along an edge. */
#define BUILD_MARK_CORNER_SHIFT 32

static char build_mark_text[BUILD_MARK_CHARS + 1];

/* Right-anchored to the upright screen's bottom-right corner, then mapped to
 * the framebuffer the way the UI's own text is. */
static void
draw_build_mark(void) {
    const int quarter = display_quarter_now();
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
        gfx_text_turned(x, y, build_mark_text, gfx_rgb(BUILD_MARK_RGB), 1, quarter);
    }
}
#endif

#if CONFIG_LAUNCHER_DEVELOPMENT
/* Report throughput on TIMER, not frames. CONFIG_LAUNCHER_DEVELOPMENT only */
static __attribute__((noinline)) void
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
        static char cost[FRAME_COST_REPORT_MAX];
        static char counts[FRAME_COST_COUNTS_MAX];
        /* Before the report, which forgets the window the counts come from. */
        if (frame_cost_take_counts(counts, sizeof counts) > 0) {
            ESP_LOGI(TAG, "%s", counts);
        }
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
/* Two lines can never both be reached once a verb and an app's prefix, or
 * two apps' own prefixes, read the same: loud here, at boot, rather than
 * silently losing one of them to whichever an unclaimed line happens to
 * match first. */
static void
check_console_prefix_clashes(void) {
    int app_count = 0;
    for (const app_t* app = app_list(); app != NULL; app = app->next) {
        app_count += app->console != NULL;
    }
    if (app_count == 0) {
        return;
    }

    const char** app_prefixes = malloc((size_t)app_count * sizeof(*app_prefixes));
    if (app_prefixes == NULL) {
        ESP_LOGE(TAG, "cannot check console prefixes: out of memory");
        while (1) {
            timing_sleep_ms(1000);
        }
    }
    int i = 0;
    for (const app_t* app = app_list(); app != NULL; app = app->next) {
        if (app->console != NULL) {
            app_prefixes[i++] = app->console->prefix;
        }
    }

    const char* from;
    const char* other;
    const console_clash_t clash = console_find_clash(console_shared(), app_prefixes, app_count, &from, &other);
    free(app_prefixes);
    switch (clash) {
        case CONSOLE_CLASH_NONE: return;
        case CONSOLE_CLASH_SPACE: ESP_LOGE(TAG, "console prefix '%s' contains a space", from); break;
        case CONSOLE_CLASH_LENGTH:
            ESP_LOGE(TAG, "console prefix '%s' plus a space does not fit CONSOLE_LINE_MAX", from);
            break;
        case CONSOLE_CLASH_VERB:
        case CONSOLE_CLASH_APP: ESP_LOGE(TAG, "console prefix '%s' clashes with '%s'", from, other); break;
    }
    while (1) {
        timing_sleep_ms(1000);
    }
}
#endif

void
shell_init(void) {
#if CONFIG_LAUNCHER_DEVELOPMENT
    const int mark_length = snprintf(build_mark_text, sizeof(build_mark_text), "D%s", build_id_short());
    if (mark_length < 0 || (size_t)mark_length >= sizeof(build_mark_text)) {
        build_mark_text[0] = '\0';
    }
#endif
    shell_apps_init();
#if CONFIG_LAUNCHER_DEVELOPMENT
    check_console_prefix_clashes();
#endif
}

#if CONFIG_LAUNCHER_SELFTEST
/* See console/console.c for framebuffer contention explanation. */
static __attribute__((noinline)) void
run_pending_selftest_suite(void) {
    const char* request = console_runsuite_take_request();
    if (request == NULL) {
        return;
    }
    const suite_run_t run = suites_run_request(request);
    if (!run.found) {
        ESP_LOGE(TAG, "no suite named '%s' is registered", run.name);
    }
    suite_report_frame_watch();
    /* On its own line, so a harness knows the suite ended without having to
     * guess from how long the console has been quiet. */
    suites_print_run(&run);
    console_runsuite_finish();
    fflush(stdout);
    /* A suite draws, clears and presents on its own, outside the shell's
     * own dirty tracking; the next real frame must repaint in full rather
     * than trust whatever a test left behind. */
    gfx_request_full_redraw();
}
#endif

static void
apply_display_orientation(int64_t now_us) {
    if (display_sample_orientation(now_us)) {
        ui_set_transform(ui_transform_quarter_turn(display_quarter_now(), GFX_WIDTH, GFX_HEIGHT));
        gfx_request_full_redraw();
    }
}

#if CONFIG_LAUNCHER_DEVELOPMENT
/* `<PREFIX>_ERR <reason>`: the app's own prefix in capitals, like a
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
console_list_apps(const app_t* current) {
    for (const app_t* app = app_list(); app != NULL; app = app->next) {
        printf("APPS name=%s running=%d\n", app->name, app == current ? 1 : 0);
    }
    printf("APPS_END\n");
    fflush(stdout);
}

static const app_t*
console_find_app(const char* prefix, bool* ambiguous) {
    const app_t* match = NULL;
    *ambiguous = false;
    for (const app_t* app = app_list(); app != NULL; app = app->next) {
        if (!console_app_name_matches(app->name, prefix)) {
            continue;
        }
        if (match != NULL) {
            *ambiguous = true;
            return NULL;
        }
        match = app;
    }
    return match;
}

/* Returns true when the request changed which app is running, so this frame
 * belongs to the switch and not to an app's frame(). */
static bool
run_console_navigation(const app_t** current, input_t* input, uint32_t dt_ms) {
    console_navigation_t navigation;
    char name[CONSOLE_LINE_MAX];
    if (!console_navigation_take_request(&navigation, name, sizeof name)) {
        return false;
    }
    const gesture_edge_t exit_edge = shell_exit_edge_for_quarter(display_quarter_now());
    if (navigation == CONSOLE_NAVIGATION_APPS) {
        console_list_apps(*current);
        return false;
    }
    if (navigation == CONSOLE_NAVIGATION_HOME) {
        if (*current != NULL) {
            shell_leave_app(current, input, exit_edge, dt_ms);
            printf("HOME_OK\n");
            fflush(stdout);
            return true;
        }
        printf("HOME_OK\n");
        fflush(stdout);
        return false;
    }
    bool ambiguous;
    const app_t* next = console_find_app(name, &ambiguous);
    if (next == NULL) {
        printf("OPEN_ERR %s '%s'\n", ambiguous ? "ambiguous" : "no app", name);
        fflush(stdout);
        return false;
    }
    if (*current == next) {
        printf("OPEN_OK name=%s\n", next->name);
        fflush(stdout);
        return false;
    }
    if (*current != NULL) {
        shell_exit_app(current);
    }
    shell_start_app(current, next);
    printf("OPEN_OK name=%s\n", next->name);
    fflush(stdout);
    return false;
}

static void
report_gesture_completion(void) {
    touch_gesture_completion_t completion;
    if (!touch_gesture_take_completion(&completion)) {
        return;
    }
    printf("%s_OK\n", touch_gesture_name(completion));
    fflush(stdout);
}

static __attribute__((noinline)) void
run_dev_frame_extras(input_t* input, const app_t* current) {
    if (gfx_mode_current()->layout == GFX_LAYOUT_FULL_FB) {
        draw_build_mark();
    }
    if (console_screenshot_take_request()) {
        console_screenshot_dump(input, current);
        gfx_request_full_redraw();
    }
    console_frame_watch_answer();
    offer_console_line(current);
}

#endif

#if CONFIG_LAUNCHER_DEVELOPMENT
static __attribute__((noinline)) bool
run_development_pre_frame(const app_t** current, input_t* input, uint32_t dt_ms) {
    report_gesture_completion();
    if (run_console_navigation(current, input, dt_ms)) {
        run_dev_frame_extras(input, *current);
        shell_present_unless_deferred(*current);
        return true;
    }
    /* The caller samples input before this runs on purpose: a held device
     * still answers the console and still latches an orientation change's
     * own full redraw, so the STEP after a rotation draws the frame that
     * rotation asked for. A held frame never reaches run_dev_frame_extras(),
     * so the line is offered here: freeze, inspect, step. */
    if (!console_freeze_frame_allowed()) {
        if (console_screenshot_take_request()) {
            console_screenshot_dump(input, *current);
        }
        console_frame_watch_answer();
        offer_console_line(*current);
        return true;
    }
    return false;
}
#endif

/* Its frame sits under every app and every suite on the main task's small
 * stack, so a helper it calls with a larger frame than its own is noinline. */
void
shell_run(void) {
    const app_t* current = NULL; /* NULL means the launcher is showing */
    input_t input = {0};
    int64_t previous_us = timing_now_us();
#if CONFIG_LAUNCHER_DEVELOPMENT
    int64_t fps_window_start = previous_us;
    uint32_t frames = 0;
#endif

    int app_count = 0;
    for (const app_t* app = app_list(); app != NULL; app = app->next) {
        app_count++;
    }
    ESP_LOGI(TAG, "Ready, %d app%s registered", app_count, app_count == 1 ? "" : "s");
    /* Again, for a host that lost the port: after a PMIC cold restart, USB
     * Serial/JTAG enumerates only about 0.7 s into the new boot, after
     * app_boot_init()'s print. */
    printf("BUILD_ID=%s\n", build_id());
    fflush(stdout);
    frame_watch_start();

    while (1) {
        const int64_t now_us = timing_now_us();
        uint32_t dt_ms = (uint32_t)((now_us - previous_us) / 1000);
        previous_us = now_us;
        if (dt_ms > FRAME_DT_MAX_MS) {
            dt_ms = FRAME_DT_MAX_MS;
        }
        FRAME_COST_BEGIN(rest_began);

#if CONFIG_LAUNCHER_SELFTEST
        run_pending_selftest_suite();
#endif

        input_poll(&input);
        apply_display_orientation(now_us);

#if CONFIG_LAUNCHER_DEVELOPMENT
        if (run_development_pre_frame(&current, &input, dt_ms)) {
            FRAME_COST_END(rest_began, "frame.rest");
            timing_yield();
            continue;
        }
#endif

        shell_step_app(&current, &input, dt_ms);
        /* Band mode has no framebuffer for an overlay to draw into. */
        if (gfx_mode_current()->layout == GFX_LAYOUT_FULL_FB) {
            shell_systems_overlay();
        }

#if CONFIG_LAUNCHER_DEVELOPMENT
        run_dev_frame_extras(&input, current);
#endif

        shell_present_unless_deferred(current);
#if CONFIG_LAUNCHER_DEVELOPMENT
        report_fps(now_us, &fps_window_start, &frames);
#endif
        FRAME_COST_END(rest_began, "frame.rest");

        /* Yield so the idle task can feed the watchdog. */
        timing_yield();
    }
}
