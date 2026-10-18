/*
 * The developer-toggles page, drawn by the real app: app_diagnostics.c's
 * enter and frame, with BOOT pressed on the first frame to page past the
 * POST report. The board's IO is stood in below, so nothing rendered here is
 * a reading from any board.
 */
#include <stdlib.h>
#include <string.h>

#include "app/app.h"
#include "display/display.h"
#include "gfx/gfx.h"
#include "gfx/present/gfx_debug.h"
#include "gfx/present/gfx_present.h"
#include "input/imu.h"
#include "render_host.h"
#include "selftest/post.h"
#include "selftest/post_ui.h"
#include "ui/layout/ui_transform.h"
#include "ui/ui.h"

#define TAP_FRAME 2
#define FRAME_MS  16
/* The second toggle row ("gfx leaf-rect overlay"), in portrait panel space. */
#define TAP_X     60
#define TAP_Y     150

static bool tap;
static int shell_quarter;
static int panel_clock_hz = GFX_PANEL_CLOCK_SLOW_HZ;
/* One toggle starts on, so the page shows both states. */
static bool overlay_on = true, leaf_on, send_audit_on;

int
display_quarter_now(void) {
    return shell_quarter;
}

int
display_system_panel_clock_hz(void) {
    return panel_clock_hz;
}

void
display_set_system_panel_clock_hz(int hz) {
    panel_clock_hz = hz;
}

/* The present path's instruments, which only a development device build has. */
void
gfx_set_debug_overlay(bool on) {
    overlay_on = on;
}

bool
gfx_debug_overlay(void) {
    return overlay_on;
}

void
gfx_set_leaf_overlay(bool on) {
    leaf_on = on;
}

bool
gfx_debug_leaf_overlay(void) {
    return leaf_on;
}

void
gfx_set_send_audit(bool on) {
    send_audit_on = on;
}

bool
gfx_send_audit(void) {
    return send_audit_on;
}

bool
imu_ready(void) {
    return false;
}

bool
imu_read(imu_sample_t* out) {
    (void)out;
    return false;
}

/* The report page is never drawn: BOOT leaves it on the first frame. */
void
post_rerun(void) {}

void
post_ui_draw_report(const post_ui_report_t* report) {
    (void)report;
    abort();
}

static bool
options(int argc, char** argv) {
    if (argc == 1 && strcmp(argv[0], "--tap") == 0) {
        tap = true;
        return true;
    }
    return argc == 0;
}

static bool
setup(int quarter) {
    shell_quarter = quarter;
    ui_init();
    ui_set_transform(ui_transform_quarter_turn(quarter, GFX_WIDTH, GFX_HEIGHT));
    app_list()->enter();
    return true;
}

static const render_input_step_t touch[] = {
    {TAP_FRAME, true, TAP_X, TAP_Y},
    {TAP_FRAME + 1, false, TAP_X, TAP_Y},
};

static void
draw(const render_frame_t* frame) {
    input_t in = tap ? frame->input : (input_t){0};
    in.boot.pressed = frame->index == 0;
    app_list()->frame(frame->dt_ms, &in);
}

const render_scene_t render_scene = {
    .name = "diagnostics",
    .frames = 6,
    .dt_ms = FRAME_MS,
    .input = touch,
    .input_count = sizeof touch / sizeof touch[0],
    .options = options,
    .setup = setup,
    .draw = draw,
};
