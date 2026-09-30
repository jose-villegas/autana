/*
 * scene_sponza: Crytek Sponza flown through on a looping camera path, as a
 * render_lab scene.
 *
 * All light is baked into vertex or face colours by tools/gen_sponza.py (a
 * sun with shadows plus sky light), so a frame is only cull, transform, clip
 * and fill: render/raster.h on both cores. Three scenes share this code, one
 * per bake: the full mesh, a lighter one and the full mesh baked flat, the
 * same flythrough through each.
 *
 * It renders at half the panel's resolution into its own PSRAM target and
 * upscales that into the framebuffer, so it asks for the full-framebuffer
 * layout.
 */

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "display/display.h"
#include "gfx/gfx.h"
#include "render/r3d.h"
#include "render_lab.h"
#include "render_lab_scene.h"
#include "render_lab_view.h"
#include "sponza_flat_mesh_generated.h"
#include "sponza_flythrough.h"
#include "sponza_lite_mesh_generated.h"
#include "sponza_mesh_generated.h"
#include "util/tune.h"

#define SKY_RGB 0x9CC0E6

static const char* TAG = "sponza";

static raster_t frame; /* holds its scratch from enter() to exit() */
static raster_stats_t stats;
static uint32_t elapsed_ms;
static bool rendered; /* update() drew a frame that frame() has not upscaled yet */

static void
enter_with(const r3d_lit_mesh_t* mesh) {
    gfx_set_partial_clear(false);
    gfx_clear(gfx_rgb(RENDER_LAB_BACKGROUND_RGB));

    frame = (raster_t){
        .mesh = mesh,
        .width = SPONZA_RENDER_WIDTH,
        .height = SPONZA_RENDER_HEIGHT,
        .clear = GFX_RGB(SKY_RGB),
        .destination = gfx_framebuffer(),
        .destination_width = GFX_WIDTH,
        .destination_height = GFX_HEIGHT,
    };
    const size_t bytes = raster_scratch_bytes(&frame);
    frame.scratch = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (frame.scratch == NULL) {
        ESP_LOGE(TAG, "no %u bytes of PSRAM for the frame: the scene stays blank", (unsigned)bytes);
    }
    elapsed_ms = 0;
    rendered = false;
}

static void
scene_sponza_enter(void) {
    enter_with(&sponza_mesh);
}

static void
scene_sponza_lite_enter(void) {
    enter_with(&sponza_lite_mesh);
}

static void
scene_sponza_flat_enter(void) {
    enter_with(&sponza_flat_mesh);
}

static void
scene_sponza_exit(void) {
    heap_caps_free(frame.scratch);
    frame.scratch = NULL;
}

/* Every frame already redraws the whole screen. */
static void
scene_sponza_invalidate(void) {}

/* Everything but the framebuffer: runs while the last frame is still
 * being sent, so it names no gfx call. */
static void
render(uint32_t dt_ms) {
    elapsed_ms += dt_ms;
    const camera_t camera = sponza_camera_at(elapsed_ms);
    stats = raster_draw(&frame, &camera, display_shell_quarter());
#if TUNE_ENABLED
    raster_show(&frame, render_lab_view());
#endif
    rendered = true;
}

static void
scene_sponza_update(uint32_t dt_ms) {
    if (frame.scratch != NULL) {
        render(dt_ms);
    }
}

static void
scene_sponza_frame(uint32_t dt_ms, bool band_mode_active) {
    assert(!band_mode_active); /* needs_full_framebuffer keeps the app out of band mode for this scene */
    if (frame.scratch == NULL) {
        return;
    }
    if (!rendered) {
        render(dt_ms); /* no update() ran since the last frame: the first after entering */
    }
    raster_upscale(&frame);
    rendered = false;
    gfx_mark_dirty(0, 0, GFX_WIDTH, GFX_HEIGHT);
}

static const char*
sponza_status(void) {
    static char buf[32];
    if (snprintf(buf, sizeof buf, "%5d tris", stats.triangles) < 0) {
        buf[0] = '\0';
    }
    return buf;
}

const render_lab_scene_t scene_sponza = {
    .name = "Sponza",
    .key = "sponza",
    .enter = scene_sponza_enter,
    .frame = scene_sponza_frame,
    .update = scene_sponza_update,
    .frame_band = NULL,
    .exit = scene_sponza_exit,
    .invalidate = scene_sponza_invalidate,
    .status = sponza_status,
    .needs_full_framebuffer = true,
    .shows_view_modes = true,
};

const render_lab_scene_t scene_sponza_lite = {
    .name = "Sponza Lite",
    .key = "sponza-lite",
    .enter = scene_sponza_lite_enter,
    .frame = scene_sponza_frame,
    .update = scene_sponza_update,
    .frame_band = NULL,
    .exit = scene_sponza_exit,
    .invalidate = scene_sponza_invalidate,
    .status = sponza_status,
    .needs_full_framebuffer = true,
    .shows_view_modes = true,
};

const render_lab_scene_t scene_sponza_flat = {
    .name = "Sponza Flat",
    .key = "sponza-flat",
    .enter = scene_sponza_flat_enter,
    .frame = scene_sponza_frame,
    .update = scene_sponza_update,
    .frame_band = NULL,
    .exit = scene_sponza_exit,
    .invalidate = scene_sponza_invalidate,
    .status = sponza_status,
    .needs_full_framebuffer = true,
    .shows_view_modes = true,
};
