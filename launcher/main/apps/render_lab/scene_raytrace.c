/*
 * scene_raytrace - the Cornell box (rt_cornell.h) as a render_lab scene.
 *
 * Progressive: frame() traces a budget of pixels straight into
 * gfx_framebuffer() and returns, so the shell's HUD keeps updating while the
 * picture resolves coarse to fine (rt_refine.h). Needs a retained framebuffer
 * to progress into - needs_full_framebuffer (render_lab_scene.h) is how it
 * tells the app to grant GFX_LAYOUT_FULL_FB whatever render_lab_band_mode
 * asks for.
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
#include "rt_refine.h"

#define TARGET_FRAME_MS     30
#define PIXEL_BUDGET_MIN    64
#define PIXEL_BUDGET_GROWTH GFX_WIDTH
#define PIXEL_BUDGET_MAX    (GFX_WIDTH * GFX_HEIGHT)

static rt_cornell_camera_t camera;
static int current_quarter;
static int step;   /* this pass's lattice spacing; 0 once the picture is done */
static int next_y; /* the next lattice row this pass has not traced */
static int pixels_per_frame;
static uint32_t elapsed_ms;

static void
restart_render(void) {
    step = RT_REFINE_FIRST_STEP;
    next_y = 0;
    pixels_per_frame = PIXEL_BUDGET_GROWTH;
    elapsed_ms = 0;
}

static void
scene_raytrace_enter(void) {
    gfx_set_partial_clear(false);
    gfx_clear(gfx_rgb(RENDER_LAB_BACKGROUND_RGB));

    current_quarter = display_shell_quarter();
    rt_cornell_camera_init(&camera, (r3d_viewport_t){GFX_WIDTH, GFX_HEIGHT, current_quarter});
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

/* dt_ms is the whole previous frame, the panel transfer included, so a
 * coarse pass that dirties many rows per traced pixel slows itself down. */
static void
adapt_pixel_budget(uint32_t last_dt_ms) {
    if (last_dt_ms > TARGET_FRAME_MS) {
        pixels_per_frame /= 2;
        if (pixels_per_frame < PIXEL_BUDGET_MIN) {
            pixels_per_frame = PIXEL_BUDGET_MIN;
        }
    } else if (pixels_per_frame < PIXEL_BUDGET_MAX) {
        pixels_per_frame += PIXEL_BUDGET_GROWTH;
    }
}

static void
fill_block(gfx_color_t* fb, int x, int y, gfx_color_t color) {
    const int w = x + step > GFX_WIDTH ? GFX_WIDTH - x : step;
    const int h = y + step > GFX_HEIGHT ? GFX_HEIGHT - y : step;

    for (int row = 0; row < h; row++) {
        gfx_color_t* dst = fb + (size_t)(y + row) * GFX_WIDTH + x;
        for (int col = 0; col < w; col++) {
            dst[col] = color;
        }
    }
}

static int
trace_lattice_row(gfx_color_t* fb, int y) {
    int traced = 0;

    for (int x = 0; x < GFX_WIDTH; x += step) {
        if (!rt_refine_is_new(x, y, step)) {
            continue;
        }
        fill_block(fb, x, y, rt_cornell_render_pixel(&camera, x, y));
        traced++;
    }
    return traced;
}

static void
draw_next_lattice_rows(void) {
    gfx_color_t* fb = gfx_framebuffer();
    const int first_y = next_y;
    int traced = 0;

    while (next_y < GFX_HEIGHT && traced < pixels_per_frame) {
        traced += trace_lattice_row(fb, next_y);
        next_y += step;
    }

    const int end_y = next_y < GFX_HEIGHT ? next_y : GFX_HEIGHT;
    gfx_mark_dirty(0, first_y, GFX_WIDTH, end_y - first_y);

    if (next_y >= GFX_HEIGHT) {
        step = rt_refine_next_step(step);
        next_y = 0;
    }
}

static void
scene_raytrace_frame(uint32_t dt_ms, bool band_mode_active) {
    assert(!band_mode_active); /* needs_full_framebuffer keeps the app out of band mode for this scene */

    const int quarter = display_shell_quarter();
    if (quarter != current_quarter) {
        current_quarter = quarter;
        rt_cornell_camera_init(&camera, (r3d_viewport_t){GFX_WIDTH, GFX_HEIGHT, quarter});
        restart_render();
    }

    if (step == 0) {
        return; /* done: nothing left to draw, nothing left to cost */
    }

    adapt_pixel_budget(dt_ms);
    draw_next_lattice_rows();
    elapsed_ms += dt_ms;
}

/* Both strings are the same length: nothing here erases a HUD box that got
 * narrower. */
static const char*
raytrace_status(void) {
    static char buf[32];

    if (step != 0) {
        snprintf(buf, sizeof buf, "pass %d/%d %3d%%", rt_refine_pass_number(step), RT_REFINE_PASSES,
                 next_y * 100 / GFX_HEIGHT);
    } else {
        snprintf(buf, sizeof buf, "done %4u.%u s", (unsigned)(elapsed_ms / 1000), (unsigned)((elapsed_ms / 100) % 10));
    }
    return buf;
}

const render_lab_scene_t scene_raytrace = {
    .name = "Cornell Box",
    .key = "cornell",
    .enter = scene_raytrace_enter,
    .frame = scene_raytrace_frame,
    .frame_band = NULL,
    .exit = scene_raytrace_exit,
    .invalidate = scene_raytrace_invalidate,
    .status = raytrace_status,
    .needs_full_framebuffer = true,
};
