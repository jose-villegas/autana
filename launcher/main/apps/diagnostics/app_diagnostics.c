/*
 * app_diagnostics - shows the POST report on demand, plus a second page of
 * developer-only toggles.
 *
 * The POST itself is silent when everything passes, which is the right default
 * for a device that should just boot. This is how you look anyway, without
 * attaching a serial cable - a board in the field usually has nothing on its
 * console.
 *
 * Entering it RE-RUNS the checks, so the report is live rather than a record of
 * what boot found - the point of opening it is usually to see whether something
 * is failing now.
 *
 * Every check is repeated, including the SD card: the re-run just re-mounts
 * the card on its own dedicated SDMMC bus, independent of the display, and
 * the report prints how long that round trip took.
 *
 * BOOT pages between the report and the toggle screen, rather than the
 * toggle screen adding a control to the report itself - the report is
 * already a dense list with no obvious room, and a second page costs
 * nothing the report's own layout has to account for.
 *
 * The toggle screen is built with microui like any other app UI (see
 * ui_launcher.c) - a real checkbox, tappable, drawn and dirty-tracked the
 * same way the launcher's own menu is - rather than hand-rolling a
 * one-off control that reads a button directly. Any future developer
 * toggle belongs on this same page as another mu_checkbox() row, not as
 * its own bespoke screen.
 */

#include <stdio.h>

#include "../../app.h"
#include "../../boot/post_ui.h"
#include "../../build_variant.h"
#include "../../display/display.h"
#include "../../gfx/gfx.h"
#include "../../input/imu.h"
#if CONFIG_LAUNCHER_SELFTEST
#include "../../boot/selftest.h"
#endif
#include "../../ui/ui.h"
#include "ui/toggles_screen.h"

#define PAGE_COUNT     2
#define COL_BACKGROUND 0x0A0C14

static int page;

/* Persisted like `page` above - a developer toggle that resets to off every
 * visit would defeat the point of leaving the board on this screen while
 * physically turning it through its holds to read the numbers off. */
static int show_orientation;

#if CONFIG_LAUNCHER_SELFTEST
/* Last selftest_run() result, persisted across frames like
 * show_orientation above rather than reset in diagnostics_enter():
 * re-running the checks on every visit already happens via
 * post_rerun() for the (cheap) POST report, but the self test suite is
 * a separate, heavier action the user explicitly asks for by tapping
 * the button below - it must not silently re-run just because the page
 * was revisited. -1 means "never tapped yet", so it reads differently
 * from a run that tapped and found zero failures. */
static int selftest_failures = -1;

/* Set by a tap of the button below; consumed at the top of
 * diagnostics_frame(), never run from inside mu_button()'s own if-block -
 * see the comment there for why. */
static bool selftest_pending;
#endif /* CONFIG_LAUNCHER_SELFTEST */

static void
diagnostics_enter(void) {
    /* Always open on the report - the page you came here for by default,
     * and the same screen every time regardless of where a previous visit
     * left off. */
    page = 0;

    /* Re-run on entry rather than per frame: the checks probe I2C and cycle the
     * audio rail, which is fine once but has no business happening 40 times a
     * second. */
    post_rerun();
}

static void
draw_toggles_page(const input_t* input) {
    mu_Context* ctx = ui_context();
    ui_begin(input);

    /* imu_read() is an I2C transaction - read only while the checkbox is
     * already on, not on the frame that turns it on (that frame shows the
     * previous reading's absence for one repaint and self-corrects the
     * next), so a page most visits never enable never pays for it. */
    toggles_screen_state_t state = {
        .overlay_on = gfx_debug_overlay(),
        .leaf_on = gfx_debug_leaf_overlay(),
        .interlace_on = gfx_interlace_enabled(),
        .fast_clock = shell_system_panel_clock_hz() == GFX_PANEL_CLOCK_FAST_HZ,
        .send_audit_on = gfx_send_audit(),
        .show_orientation = show_orientation,
        .imu_ready = imu_ready(),
        .shell_quarter = display_shell_quarter(),
#if CONFIG_LAUNCHER_SELFTEST
        .selftest_failures = selftest_failures,
#endif
    };

    if (state.show_orientation && state.imu_ready) {
        imu_sample_t sample;
        state.have_sample = imu_read(&sample);
        if (state.have_sample) {
            state.accel_ax = sample.ax;
            state.accel_ay = sample.ay;
            state.accel_az = sample.az;
            state.gravity_gx = imu_gravity_screen_x(&sample);
            state.gravity_gy = imu_gravity_screen_y(&sample);
        }
    }

    const toggles_screen_result_t result = toggles_screen_draw(ctx, &state);

    gfx_set_debug_overlay(result.overlay_on);
    gfx_set_leaf_overlay(result.leaf_on);
    gfx_set_interlace(result.interlace_on);
    shell_set_system_panel_clock_hz(result.fast_clock ? GFX_PANEL_CLOCK_FAST_HZ : GFX_PANEL_CLOCK_SLOW_HZ);
    gfx_set_send_audit(result.send_audit_on);
    show_orientation = result.show_orientation;
#if CONFIG_LAUNCHER_SELFTEST
    /* Only FLAGGED here, not run: selftest_run() itself happens at the top
     * of diagnostics_frame(), outside this page's own ui_begin()/ui_end()
     * bracket - see the comment there for why it cannot run from inside
     * this call. */
    if (result.selftest_clicked) {
        selftest_pending = true;
    }
#endif

    ui_end(COL_BACKGROUND);
}

static void
diagnostics_frame(uint32_t dt_ms, const input_t* input) {
    (void)dt_ms;

#if CONFIG_LAUNCHER_SELFTEST
    /* Consumed here, before this frame's own ui_begin()/ui_end() bracket
     * opens - never from inside mu_button()'s own if-block in
     * draw_toggles_page(). selftest_run() runs suite_ui.c, whose
     * fixture() calls ui_init()/mu_init() on the same ui_context()
     * singleton every window in this shell draws through; running it
     * synchronously mid-frame would stomp state ui_end() further down
     * still relies on. */
    if (selftest_pending) {
        selftest_pending = false;
        selftest_failures = selftest_run();
        /* selftest_run()'s own tests set ui_set_transform() to a fixed
         * sequence of quarter-turns ending wherever the LAST test left
         * it, not the board's real orientation - restored here
         * immediately, one frame of latency before draw_toggles_page()
         * ever opens its own frame. */
        ui_set_transform(ui_transform_quarter_turn(display_shell_quarter(), GFX_WIDTH, GFX_HEIGHT));
    }
#endif /* CONFIG_LAUNCHER_SELFTEST */

    if (input->boot.pressed) {
        page = (page + 1) % PAGE_COUNT;
        if (page == 1) {
            /* Switching in from the report page, drawn entirely outside
             * microui - ui_end() compares this page's own command list
             * against its last paint and would otherwise see no change
             * and skip repainting, leaving the report's pixels on screen
             * underneath. See ui_invalidate()'s own comment. */
            ui_invalidate();
        }
    }

    /* Redrawn every frame rather than cached: the shell owns the framebuffer
     * and the previous app may have left anything in it. */
    if (page == 0) {
        post_ui_draw_report("POWER-ON SELF TEST");
    } else {
        draw_toggles_page(input);
    }
}

static void
diagnostics_exit(void) {}

const app_t app_diagnostics = {
    .name = "Diagnostics",
    .summary = "Hardware self-test report",
    .enter = diagnostics_enter,
    .frame = diagnostics_frame,
    .exit = diagnostics_exit,
    /* No cache of its own beyond ui.c's shared one - see ui_invalidate()'s
     * own comment for why a repaint replacing the screen out from under it
     * needs this. */
    .invalidate = ui_invalidate,
    .home_gesture = true,
};

APP_REGISTER(app_diagnostics);
