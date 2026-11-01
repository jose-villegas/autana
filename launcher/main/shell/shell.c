/*
 * The frame loop, the one on the device: it lives here, not in the apps, so
 * switching is instant and no app can wedge the system by failing to yield.
 */

#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "app/app.h"
#include "core/timing.h"
#include "display/display.h"
#include "display/display_shell.h"
#include "draw/draw.h"
#include "draw/draw_font_roles.h"
#include "gfx/gfx.h"
#include "gfx/present/gfx_debug.h"
#include "gfx/present/gfx_mode.h"
#include "gfx/present/gfx_present.h"
#include "input/input.h"
#include "input/input_shell.h"
#include "input/touch/gesture.h"
#include "input/touch/touch.h"
#include "profile/frame_cost.h"
#include "profile/frame_laps.h"
#include "profile/frame_watch.h"
#include "services/build_id.h"
#include "shell/shell.h"
#include "shell/shell_apps.h"
#include "shell/shell_frame.h"
#include "ui/ui.h"

#if CONFIG_LAUNCHER_DEVELOPMENT
#include "console/console.h"
#include "console/console_frame_request.h"
#include "console/console_verbs.h"
#include "shell/console/console_freeze.h"
#include "shell/console/console_navigation.h"
#include "shell/console/console_navigation_parse.h"
#include "shell/console/console_screenshot.h"
#endif

#if CONFIG_LAUNCHER_SELFTEST
#include "selftest/console_runsuite.h"
#include "suites.h"
#include "unity.h"
#endif

#include "esp_log.h"

static const char* TAG = "shell";

/* A stall must not reach an app as one long step. */
#define FRAME_DT_MAX_MS 250

#if CONFIG_LAUNCHER_DEVELOPMENT
/* Report throughput on TIMER, not frames. CONFIG_LAUNCHER_DEVELOPMENT only */
#define SHELL_REPORT_WINDOW_US 1500000

static __attribute__((noinline)) void
report_fps(int64_t now_us, int64_t* window_start, uint32_t* frames) {
    (*frames)++;

    /* Loop passes alone overstate smoothness: an idle pass sends nothing.
     * Presents that sent a strip, and the touch samples feeding them, are
     * what a swipe actually sees. */
    static uint32_t drawn;
    int full = 0;
    int gathered = 0;
    int partial = 0;
    gfx_get_strip_send_counts(&full, &gathered, &partial);
    if (full + gathered + partial > 0) {
        drawn++;
        gfx_reset_strip_send_counts();
    }

    const int64_t since = now_us - *window_start;
    if (since >= SHELL_REPORT_WINDOW_US) {
        uint32_t points = 0;
        uint32_t moved = 0;
        touch_take_sample_counts(&points, &moved);
        unsigned reads = 0;
        unsigned failed = 0;
        int64_t read_p95_us = 0;
        int64_t read_max_us = 0;
        touch_take_read_times(&reads, &failed, &read_p95_us, &read_max_us);
        const double per_s = 1000000.0 / (double)since;
        ESP_LOGI(TAG,
                 "%.1f fps, %.1f drawn/s, touch %.1f points/s %.1f moved/s, reads %u failed %u read_p95_us %lld "
                 "read_max_us %lld",
                 (double)*frames * per_s, (double)drawn * per_s, (double)points * per_s, (double)moved * per_s, reads,
                 failed, (long long)read_p95_us, (long long)read_max_us);
        static char cost[FRAME_COST_REPORT_MAX];
        static char counts[FRAME_COST_COUNTS_MAX];
        /* Before the report, which forgets the window the counts come from. */
        if (frame_cost_take_counts(counts, sizeof counts) > 0) {
            ESP_LOGI(TAG, "%s", counts);
        }
        if (frame_cost_take_report(*frames, cost, sizeof cost) > 0) {
            ESP_LOGI(TAG, "ms/frame avg/worst: %s", cost);
        }
#if FRAME_LAPS_ENABLED
        static char laps[FRAME_LAPS_REPORT_MAX];
        if (frame_laps_take_report(*frames, laps, sizeof laps) > 0) {
            ESP_LOGI(TAG, "ms/frame laps: %s", laps);
        }
#endif
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
    shell_frame_init(build_id_short());
#endif
    shell_apps_init();
}

void
shell_check_console_prefixes(void) {
#if CONFIG_LAUNCHER_DEVELOPMENT
    check_console_prefix_clashes();
#endif
}

#if CONFIG_LAUNCHER_SELFTEST
/* See console/console.c for framebuffer contention explanation. */
static __attribute__((noinline)) void
run_pending_selftest_suite(void) {
    if (!console_frame_take(console_frame_slots(), &console_runsuite_slot)) {
        return;
    }
    const console_runsuite_request_t* request = console_runsuite_slot.taken;
    static suite_run_t run;
    UNITY_BEGIN();
    suites_run_request(request->suite, &run);
    if (!run.found) {
        ESP_LOGE(TAG, "no suite named '%s' is registered", run.name);
    }
    suite_report_frame_watch();
    (void)UNITY_END();
    /* On its own line, so a harness knows the suite ended without having to
     * guess from how long the console has been quiet. */
    suites_print_run(&run);
    console_frame_done(console_frame_slots(), &console_runsuite_slot);
    (void)fflush(stdout);
    /* A suite draws, clears and presents on its own, outside the shell's
     * own dirty tracking; the next real frame must repaint in full rather
     * than trust whatever a test left behind. */
    gfx_request_full_redraw();
}
#endif

static void
apply_display_orientation(int64_t now_us) {
    if (display_sample_orientation(now_us, input_read_acceleration)) {
        ui_set_transform(ui_transform_quarter_turn(display_quarter_now(), GFX_WIDTH, GFX_HEIGHT));
        gfx_request_full_redraw();
    }
}

#if CONFIG_LAUNCHER_DEVELOPMENT
/* `<PREFIX>_ERR <reason>`: the app's own prefix in capitals, like a
 * verb's own TUNE_ERR, so a caller matching `<PREFIX>_ERR`
 * (docs/cli/README.md) recognises either failure below the same way
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
    (void)fflush(stdout);
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
        const char* prefix = app->console != NULL ? app->console->prefix : NULL;
        printf("APPS name=%s running=%d%s%s\n", app->name, app == current ? 1 : 0, prefix != NULL ? " console=" : "",
               prefix != NULL ? prefix : "");
    }
    printf("APPS_END\n");
    (void)fflush(stdout);
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
run_console_navigation(const app_t** current, input_t* input, uint32_t dt_ms, bool navigate) {
    if (!navigate) {
        return false;
    }
    const console_navigation_request_t* request = console_navigation_slot.taken;
    const console_navigation_t navigation = request->navigation;
    const char* name = request->app;
    const gesture_edge_t exit_edge = shell_exit_edge_for_quarter(display_quarter_now());
    if (navigation == CONSOLE_NAVIGATION_APPS) {
        console_list_apps(*current);
        return false;
    }
    if (navigation == CONSOLE_NAVIGATION_HOME) {
        if (*current != NULL) {
            shell_leave_app(current, input, exit_edge, dt_ms);
            printf("HOME_OK\n");
            (void)fflush(stdout);
            return true;
        }
        printf("HOME_OK\n");
        (void)fflush(stdout);
        return false;
    }
    bool ambiguous;
    const app_t* next = console_find_app(name, &ambiguous);
    if (next == NULL) {
        printf("OPEN_ERR %s '%s'\n", ambiguous ? "ambiguous" : "no app", name);
        (void)fflush(stdout);
        return false;
    }
    if (*current == next) {
        printf("OPEN_OK name=%s\n", next->name);
        (void)fflush(stdout);
        return false;
    }
    if (*current != NULL) {
        shell_exit_app(current);
    }
    shell_start_app(current, next);
    printf("OPEN_OK name=%s\n", next->name);
    (void)fflush(stdout);
    return false;
}

static void
report_gesture_completion(void) {
    touch_inject_completion_t completion;
    if (!touch_inject_take_completion(&completion)) {
        return;
    }
    printf("%s_OK\n", touch_inject_completion_name(completion));
    (void)fflush(stdout);
}

static __attribute__((noinline)) void
run_dev_frame_extras(input_t* input, const app_t* current, bool screenshot) {
    shell_frame_extras();
    if (screenshot) {
        console_screenshot_dump(input, current);
        gfx_request_full_redraw();
    }
    offer_console_line(current);
}

#endif

#if CONFIG_LAUNCHER_DEVELOPMENT
static __attribute__((noinline)) bool
run_development_pre_frame(const app_t** current, input_t* input, uint32_t dt_ms, bool navigate, bool screenshot) {
    report_gesture_completion();
    if (run_console_navigation(current, input, dt_ms, navigate)) {
        run_dev_frame_extras(input, *current, screenshot);
        shell_present_unless_deferred(*current);
        return true;
    }
    /* The caller samples input before this runs on purpose: a held device
     * still answers the console and still latches an orientation change's
     * own full redraw, so the STEP after a rotation draws the frame that
     * rotation asked for. A held frame never reaches run_dev_frame_extras(),
     * so the line is offered here: freeze, inspect, step. */
    if (!console_freeze_frame_allowed()) {
        if (screenshot) {
            console_screenshot_dump(input, *current);
        }
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
    static input_t input;
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
    /* Again, for a host that lost the port: after the PSRAM tuning's unplug
     * (board/board_usb.c), USB Serial/JTAG enumerates again only after
     * app_boot_init()'s print. */
    printf("BUILD_ID=%s\n", build_id());
    (void)fflush(stdout);
    frame_watch_start();

    while (1) {
        const int64_t now_us = timing_now_us();
        uint32_t dt_ms = (uint32_t)((now_us - previous_us) / 1000);
        previous_us = now_us;
        if (dt_ms > FRAME_DT_MAX_MS) {
            dt_ms = FRAME_DT_MAX_MS;
        }
        FRAME_COST_BEGIN(rest_began);

#if CONFIG_LAUNCHER_DEVELOPMENT
        /* Before the freeze gate and every early return below: what those
         * would skip is applied here, so a held frame still answers. */
        /* A capture first, then what it may depend on: anything posted
         * before a SCREENSHOT this pass takes is applied before it runs. */
        const bool screenshot = console_frame_take(console_frame_slots(), &console_screenshot_slot);
        const bool navigate = console_frame_take(console_frame_slots(), &console_navigation_slot);
        console_frame_answer_pending(console_frame_slots());
#endif
#if CONFIG_LAUNCHER_SELFTEST
        run_pending_selftest_suite();
#endif

        input_poll(&input);
        if (input.down) {
            display_touch_now(now_us);
        }
        apply_display_orientation(now_us);

#if CONFIG_LAUNCHER_DEVELOPMENT
        if (run_development_pre_frame(&current, &input, dt_ms, navigate, screenshot)) {
            FRAME_COST_END(rest_began, "frame.rest");
            timing_yield();
            continue;
        }
#endif

        shell_step_app(&current, &input, dt_ms);

#if CONFIG_LAUNCHER_DEVELOPMENT
        run_dev_frame_extras(&input, current, screenshot);
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
