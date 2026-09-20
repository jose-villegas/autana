/*
 * scene_raytrace - the Cornell box (rt_cornell.h) as a render_lab scene.
 *
 * Progressive: frame() traces the next few rows straight into
 * gfx_framebuffer() and returns, so the shell's HUD keeps updating while the
 * picture fills in top to bottom. Needs a retained framebuffer to progress
 * into - needs_full_framebuffer (render_lab_scene.h) is how it tells the app
 * to grant GFX_LAYOUT_FULL_FB whatever render_lab_band_mode asks for.
 */

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "../../display/display.h"
#include "../../gfx/gfx.h"
#include "render_lab.h"
#include "render_lab_scene.h"
#include "rt_cornell.h"

#define TARGET_FRAME_MS 30

static rt_cornell_camera_t camera;
static int current_quarter;
static int next_row;
static int rows_per_frame;
static uint32_t elapsed_ms;

static void
restart_render(void) {
    next_row = 0;
    rows_per_frame = 1;
    elapsed_ms = 0;
}

static void
scene_raytrace_enter(void) {
    gfx_set_partial_clear(false);
    gfx_clear(gfx_rgb(RENDER_LAB_BACKGROUND_RGB));

    current_quarter = display_shell_quarter();
    rt_cornell_camera_init(&camera, GFX_WIDTH, GFX_HEIGHT, current_quarter);
    restart_render();
}

static void
scene_raytrace_exit(void) {
    /* Nothing allocated by scene_raytrace_enter() beyond static storage. */
}

/* The shell asking for a full redraw, and a shell orientation change
 * (checked every frame below, since this scene draws directly into panel
 * pixels rather than through a rotation-aware layer), both restart the
 * trace: a full redraw already fires on an orientation change, so the two
 * triggers overlap rather than compose. */
static void
scene_raytrace_invalidate(void) {
    restart_render();
}

static void
adapt_rows_per_frame(uint32_t last_dt_ms) {
    if (last_dt_ms > TARGET_FRAME_MS) {
        rows_per_frame /= 2;
        if (rows_per_frame < 1) {
            rows_per_frame = 1;
        }
    } else if (rows_per_frame < GFX_HEIGHT) {
        rows_per_frame += 1;
    }
}

static void
draw_next_rows(void) {
    const int rows_left = GFX_HEIGHT - next_row;
    const int count = rows_per_frame < rows_left ? rows_per_frame : rows_left;
    gfx_color_t* fb = gfx_framebuffer();

    for (int i = 0; i < count; i++) {
        rt_cornell_render_row(&camera, next_row + i, fb + (size_t)(next_row + i) * GFX_WIDTH);
    }
    gfx_mark_dirty(0, next_row, GFX_WIDTH, count);
    next_row += count;
}

static void
scene_raytrace_frame(uint32_t dt_ms, bool band_mode_active) {
    assert(!band_mode_active); /* needs_full_framebuffer keeps the app out of band mode for this scene */

    const int quarter = display_shell_quarter();
    if (quarter != current_quarter) {
        current_quarter = quarter;
        rt_cornell_camera_init(&camera, GFX_WIDTH, GFX_HEIGHT, quarter);
        restart_render();
    }

    if (next_row >= GFX_HEIGHT) {
        return; /* done: nothing left to draw, nothing left to cost */
    }

    adapt_rows_per_frame(dt_ms);
    draw_next_rows();
    elapsed_ms += dt_ms;
}

static const char*
raytrace_status(void) {
    static char buf[24];

    if (next_row < GFX_HEIGHT) {
        snprintf(buf, sizeof buf, "row  %3d/%d", next_row, GFX_HEIGHT);
    } else {
        snprintf(buf, sizeof buf, "done %3u.%u s", (unsigned)(elapsed_ms / 1000), (unsigned)((elapsed_ms / 100) % 10));
    }
    return buf;
}

const render_lab_scene_t scene_raytrace = {
    .name = "Cornell Box",
    .enter = scene_raytrace_enter,
    .frame = scene_raytrace_frame,
    .frame_band = NULL,
    .exit = scene_raytrace_exit,
    .invalidate = scene_raytrace_invalidate,
    .status = raytrace_status,
    .needs_full_framebuffer = true,
};
