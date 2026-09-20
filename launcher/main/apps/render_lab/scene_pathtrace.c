/*
 * scene_pathtrace - the Cornell box (rt_path.h) as a render_lab scene: an
 * accumulating path tracer sharing rt_cornell.h's camera and room.
 *
 * Progressive in two phases, both driven by rt_path_schedule_advance():
 * every pixel is seeded with the direct-light estimate through rt_refine.h's
 * lattice, the same coarse-to-fine reveal scene_raytrace.c uses; then whole
 * rows, top to bottom, fold one more path sample into the running mean per
 * full sweep of the screen. needs_full_framebuffer (render_lab_scene.h) is
 * how it tells the app to grant GFX_LAYOUT_FULL_FB whatever
 * render_lab_band_mode asks for.
 *
 * The accumulator is allocated, not a static: a permanent ~1 MB .bss entry
 * has twice eaten this project's dev-build heap margin already. A failed
 * allocation falls back to rt_path_schedule_advance()'s own direct-light-only
 * path rather than losing the scene.
 */

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_heap_caps.h"

#include "../../display/display.h"
#include "../../gfx/gfx.h"
#include "render_lab.h"
#include "render_lab_scene.h"
#include "rt_cornell.h"
#include "rt_path.h"
#include "rt_pixel_budget.h"
#include "rt_refine.h"

#define TARGET_FRAME_MS     30
#define PIXEL_BUDGET_MIN    64
#define PIXEL_BUDGET_GROWTH GFX_WIDTH
#define PIXEL_BUDGET_MAX    (GFX_WIDTH * GFX_HEIGHT)

static rt_cornell_camera_t camera;
static int current_quarter;
static rt_path_schedule_t schedule;
static rt_path_accum_px_t* accum; /* NULL: the allocation failed, direct light only */
static rt_pixel_budget_t pixel_budget;

static void
clear_accum(void) {
    if (accum == NULL) {
        return;
    }
    for (int i = 0; i < GFX_WIDTH * GFX_HEIGHT; i++) {
        accum[i] = (rt_path_accum_px_t){0, 0, 0};
    }
}

static void
restart_render(void) {
    rt_path_schedule_reset(&schedule);
    clear_accum();
    pixel_budget = rt_pixel_budget_init(PIXEL_BUDGET_GROWTH, PIXEL_BUDGET_MIN, PIXEL_BUDGET_MAX, TARGET_FRAME_MS);
}

static void
scene_pathtrace_enter(void) {
    gfx_set_partial_clear(false);
    gfx_clear(gfx_rgb(RENDER_LAB_BACKGROUND_RGB));

    accum = heap_caps_malloc(sizeof(*accum) * (size_t)GFX_WIDTH * GFX_HEIGHT, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    current_quarter = display_shell_quarter();
    rt_cornell_camera_init(&camera, (r3d_viewport_t){GFX_WIDTH, GFX_HEIGHT, current_quarter});
    restart_render();
}

static void
scene_pathtrace_exit(void) {
    heap_caps_free(accum);
    accum = NULL;
}

/* Same trigger as scene_raytrace_invalidate(): a full redraw or a shell
 * orientation change (checked every frame below, since this scene draws
 * directly into panel pixels) restarts the accumulation - a converged
 * frame under the old pose is not a sample of the new one. */
static void
scene_pathtrace_invalidate(void) {
    restart_render();
}

static void
scene_pathtrace_frame(uint32_t dt_ms, bool band_mode_active) {
    assert(!band_mode_active); /* needs_full_framebuffer keeps the app out of band mode for this scene */

    const int quarter = display_shell_quarter();
    if (quarter != current_quarter) {
        current_quarter = quarter;
        rt_cornell_camera_init(&camera, (r3d_viewport_t){GFX_WIDTH, GFX_HEIGHT, quarter});
        restart_render();
    }

    /* dt_ms is the whole previous frame, the panel transfer included, so a
     * sweep that dirties many rows per traced pixel slows itself down. */
    rt_pixel_budget_adapt(&pixel_budget, dt_ms);

    const rt_path_target_t target = {gfx_framebuffer(), accum, GFX_WIDTH, GFX_HEIGHT};
    const rt_path_span_t span = rt_path_schedule_advance(&schedule, &camera, target, pixel_budget.value);
    if (span.y1 > span.y0) {
        gfx_mark_dirty(0, span.y0, GFX_WIDTH, span.y1 - span.y0);
    }
}

/* Every branch is the same length: nothing here erases a HUD box that got
 * narrower. */
static const char*
pathtrace_status(void) {
    static char buf[32];

    if (schedule.step != 0) {
        snprintf(buf, sizeof buf, "seed %d/%d %3d%%", rt_refine_pass_number(schedule.step), RT_REFINE_PASSES,
                 schedule.seed_y * 100 / GFX_HEIGHT);
    } else if (accum == NULL) {
        snprintf(buf, sizeof buf, "%-13s", "direct only");
    } else {
        snprintf(buf, sizeof buf, "spp %9u", (unsigned)schedule.spp);
    }
    return buf;
}

const render_lab_scene_t scene_pathtrace = {
    .name = "Cornell Box PT",
    .key = "cornell-pt",
    .enter = scene_pathtrace_enter,
    .frame = scene_pathtrace_frame,
    .frame_band = NULL,
    .exit = scene_pathtrace_exit,
    .invalidate = scene_pathtrace_invalidate,
    .status = pathtrace_status,
    .needs_full_framebuffer = true,
};
