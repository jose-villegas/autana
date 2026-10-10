/*
 * The firmware that runs on the board: a shell that launches apps and draws
 * every frame itself. This is its entry point; it starts the hardware in a
 * fixed order, then hands the task to the shell (shell/shell.h), which never
 * gives it back.
 */

#include <stdbool.h>
#include <stdio.h>

#include "boot/boot_anim.h"
#include "boot/boot_report.h"
#include "build/build_variant.h"
#include "core/memory.h"
#include "core/timing.h"
#include "display/display.h"
#include "display/display_shell.h"
#include "gfx/gfx.h"
#include "gfx/present/gfx_present.h"
#include "input/input_shell.h"
#include "selftest/post.h"
#include "selftest/post_layout.h"
#include "selftest/post_ui.h"
#include "services/build_id.h"
#include "shell/shell.h"
#include "ui/ui.h"
#include "ui/ui_launcher.h"

#if CONFIG_LAUNCHER_DEVELOPMENT
#include "console/console.h"
#endif

#if CONFIG_LAUNCHER_SELFTEST
#include "selftest/selftest.h"
#endif

#include "esp_log.h"

static const char* TAG = "shell";

#if CONFIG_LAUNCHER_DEVELOPMENT
/* Free heap alone never predicts whether the next big allocation fits:
 * the framebuffer and an app's largest buffer each need ONE CONTIGUOUS block,
 * and
 * free space can sit outside the largest one with nothing saying where
 * it went. Printing both numbers at each boot phase says which phase
 * loses it. */
static void
heap_mark(const char* where) {
    ESP_LOGI(TAG, "HEAPMARK %-18s free %6u largest %6u", where, (unsigned)memory_free_bytes(MEMORY_DMA),
             (unsigned)memory_largest_block(MEMORY_DMA));
}
#else
#define heap_mark(where) ((void)0)
#endif

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
    timing_sleep_ms(8000);
}

/* Park rather than return on graphics failure: returning from app_main
 * leaves the chip idle and unflashable. */
static __attribute__((noinline)) void
app_boot_init(void) {
    printf("BUILD_ID=%s\n", build_id());
    fflush(stdout);
    shell_init();
    heap_mark("boot");

    /* Test SD card during panel use. */
    post_run_before_display();
    heap_mark("after sd probe");

    if (!display_start()) {
        ESP_LOGE(TAG, "Graphics failed to start; nothing more to do");
        while (1) {
            timing_sleep_ms(1000);
        }
    }

    heap_mark("after gfx_init");
    display_load_panel_clock();

    if (!post_run_after_display()) {
        show_post_failures();
    }
    heap_mark("after post");
#if CONFIG_LAUNCHER_DEVELOPMENT
    memory_dump(MEMORY_DMA);
#endif

#if CONFIG_LAUNCHER_SELFTEST && CONFIG_LAUNCHER_SELFTEST_AUTORUN
    if (selftest_run() != 0) {
        ESP_LOGE(TAG, "self test reported failures");
    }
#endif

    shell_check_console_prefixes();
    /* The launcher has to exist, turned the way boot draws, before the
     * animation can dissolve into it. ui_init() resets the transform to
     * identity, so DISPLAY_DEFAULT_QUARTER is applied here or the board
     * would start upright and visibly turn into place. */
    display_reset_quarter();
    ui_launcher_init();
    ui_set_transform(ui_transform_quarter_turn(display_quarter_now(), GFX_WIDTH, GFX_HEIGHT));

    boot_anim_set_ending_backdrop(shell_paint_home_under_boot);
    boot_anim_run();
    gfx_request_full_redraw();
    heap_mark("after boot anim");

    input_start();
#if CONFIG_LAUNCHER_DEVELOPMENT
    console_start();
#endif
    heap_mark("shell ready");
    /* Last, not first: the host's port is still re-enumerating after the
     * PMIC power cycle while the first second of boot is printed. */
    boot_report_last_reset();
}

void
app_main(void) {
    app_boot_init();
    shell_run();
}