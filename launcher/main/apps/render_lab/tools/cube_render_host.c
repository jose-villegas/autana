/*
 * cube_render_host - this app itself, entered and stepped on a host, with
 * its real enter()/frame() and the real gfx and ui layers underneath.
 *
 * A render_host.h scene. app_*.c is the hardware-facing entry point and is
 * excluded from the host test runner for that reason, but this one asks
 * nothing of the board: no IDF header, no timer, no allocator of its own.
 * What it does need is the shell, so the two shell-owned functions below
 * stand in - a registry of exactly the app that registered itself, and no
 * frame loop at all.
 *
 * Time and touch come from the harness, never from a clock or a finger, so
 * the same arguments always produce the same pixels.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "app.h"
#include "gfx/gfx.h"
#include "render_host.h"
#include "ui/ui.h"
#include "ui/ui_transform.h"

/* The band ring keeps no retained frame for render_host.c to read back,
 * and a device capture refuses it for the same reason, so this asks for the
 * full-framebuffer layout the way suite_cube_band_perf.c asks for the other
 * one. */
extern bool render_lab_band_mode;

static const app_t* registered;

void
app_register(const app_t* app) {
    registered = app;
}

const app_t* const*
app_list(void) {
    return &registered;
}

int
app_list_count(void) {
    return registered != NULL ? 1 : 0;
}

static bool
setup(int quarter) {
    if (registered == NULL || registered->enter == NULL || registered->frame == NULL) {
        fprintf(stderr, "no app registered itself\n");
        return false;
    }
    render_lab_band_mode = false;
    ui_init();
    ui_set_transform(ui_transform_quarter_turn(quarter, GFX_WIDTH, GFX_HEIGHT));
    registered->enter();
    return true;
}

static void
draw(const render_frame_t* frame) {
    registered->frame(frame->dt_ms, &frame->input);
}

const render_scene_t render_scene = {
    .name = "cube",
    .quarter = 1,
    .frames = 30,
    .dt_ms = 16,
    .setup = setup,
    .draw = draw,
};
