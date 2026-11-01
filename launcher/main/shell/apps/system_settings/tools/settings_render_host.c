/* The settings list drawn by the real app, with stand-in owner values and touch. */
#include <stdlib.h>
#include <string.h>

#include "app/app.h"
#include "display/display.h"
#include "gfx/gfx.h"
#include "gfx/present/gfx_debug.h"
#include "gfx/present/gfx_present.h"
#include "render_host.h"
#include "ui/layout/ui_transform.h"
#include "ui/ui.h"

#define TAP_FRAME 2
#define FRAME_MS  16
/* Inside the panel-clock row, the first under the DISPLAY heading, in
 * portrait panel space. */
#define TAP_X     (UI_ROW_HEIGHT / 2)
#define TAP_Y     (UI_TITLE_HEIGHT + (UI_ROW_HEIGHT / 2))

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
draw(const provider_frame_t* frame) {
    input_t in = tap ? frame->input : (input_t){0};
    app_list()->frame(frame->dt_ms, &in);
}

const frame_provider_t settings_provider = {
    .name = "settings",
    .frames = 6,
    .dt_ms = FRAME_MS,
    .input = touch,
    .input_count = sizeof touch / sizeof touch[0],
    .options = options,
    .setup = setup,
    .draw = draw,
};
