/*
 * scene_sponza - Crytek Sponza flown through on a looping camera path, as a
 * render_lab scene.
 *
 * All light is baked into vertex colours by tools/gen_sponza.py (a sun with
 * shadows plus sky light), so a frame is only cull, transform, clip and
 * fill: lit_frame.h over sponza_mesh_generated.h, on both cores.
 *
 * It renders at half the panel's resolution into its own PSRAM target and
 * doubles that into the framebuffer, so it asks for the full-framebuffer
 * layout.
 */

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_heap_caps.h"

#include "display/display.h"
#include "gfx/gfx.h"
#include "lit_frame.h"
#include "render_lab.h"
#include "render_lab_scene.h"
#include "sponza_flythrough.h"
#include "sponza_mesh_generated.h"

#define SKY_RGB            0x9CC0E6
#define HALF_FOV_SHORT_TAN 0.62f
#define NEAR_Z             6.0f

/* Rendered at half the panel's resolution in each axis, then doubled. */
#define RENDER_WIDTH       (GFX_WIDTH / 2)
#define RENDER_HEIGHT      (GFX_HEIGHT / 2)
#define RENDER_PIXELS      ((size_t)RENDER_WIDTH * RENDER_HEIGHT)

static void* scratch;
static void* target; /* the half-size colour, then the half-size depth */
static lit_frame_stats_t stats;
static uint32_t elapsed_ms;

static void
scene_sponza_enter(void) {
    gfx_set_partial_clear(false);
    gfx_clear(gfx_rgb(RENDER_LAB_BACKGROUND_RGB));

    scratch = heap_caps_malloc(lit_frame_scratch_bytes(&sponza_mesh), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    target =
        heap_caps_malloc(RENDER_PIXELS * (sizeof(gfx_color_t) + sizeof(uint16_t)), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    elapsed_ms = 0;
}

static void
scene_sponza_exit(void) {
    heap_caps_free(scratch);
    heap_caps_free(target);
    scratch = NULL;
    target = NULL;
}

/* Every frame already redraws the whole screen. */
static void
scene_sponza_invalidate(void) {}

static void
scene_sponza_frame(uint32_t dt_ms, bool band_mode_active) {
    assert(!band_mode_active); /* needs_full_framebuffer keeps the app out of band mode for this scene */
    if (scratch == NULL || target == NULL) {
        return;
    }

    elapsed_ms += dt_ms;
    lit_vec3_t eye, forward;
    camera_path_sample(&sponza_flythrough, elapsed_ms, &eye, &forward);
    lit_view_t view;
    lit_view_look(&view, eye, forward, HALF_FOV_SHORT_TAN, NEAR_Z, SPONZA_POSITION_SCALE, RENDER_WIDTH, RENDER_HEIGHT,
                  display_shell_quarter());

    gfx_color_t* color = target;
    lit_frame_t frame = {
        .mesh = &sponza_mesh,
        .color = color,
        .depth = (uint16_t*)(color + RENDER_PIXELS),
        .width = RENDER_WIDTH,
        .height = RENDER_HEIGHT,
        .clear = gfx_rgb(SKY_RGB),
        .doubled = gfx_framebuffer(),
    };
    lit_frame_use_scratch(&frame, scratch);
    stats = lit_frame_render(&frame, &view);
    gfx_mark_dirty(0, 0, GFX_WIDTH, GFX_HEIGHT);
}

static const char*
sponza_status(void) {
    static char buf[32];
    snprintf(buf, sizeof buf, "%5d tris", stats.triangles);
    return buf;
}

const render_lab_scene_t scene_sponza = {
    .name = "Sponza",
    .key = "sponza",
    .enter = scene_sponza_enter,
    .frame = scene_sponza_frame,
    .frame_band = NULL,
    .exit = scene_sponza_exit,
    .invalidate = scene_sponza_invalidate,
    .status = sponza_status,
    .needs_full_framebuffer = true,
};
