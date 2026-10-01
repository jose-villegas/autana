/*
 * scene_sponza: Crytek Sponza flown through on a looping camera path.
 *
 * Light is baked into the mesh, so a frame is only cull, transform, clip and
 * fill on both cores. Three scenes share this code, one per bake: full, lite
 * and flat. It renders into its own PSRAM target and upscales into the
 * framebuffer.
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
#include "sponza_flythrough.h"
#include "sponza_scene_generated.h"
#include "util/tune.h"

#define SKY_RGB 0x9CC0E6

static const char* TAG = "sponza";

static raster_t raster; /* holds its scratch from enter() to exit() */
static raster_stats_t stats;
static uint32_t elapsed_ms;
static bool rendered; /* update() drew the raster, which frame() has not upscaled yet */

/* What the panel says in place of the triangle count when the scene could not
 * open its meshes; empty when it could. */
static char failure[48];

/* Points the scene's three instances at their meshes in the asset pack. */
static bool
bind_meshes(void) {
    const char* failed = NULL;
    const asset_status_t status = sponza_open_meshes(&failed);
    failure[0] = '\0';
    if (status == ASSET_OK) {
        return true;
    }
    ESP_LOGE(TAG, "mesh '%s': %s; the scene stays blank", failed == NULL ? "?" : failed, asset_status_text(status));
    const bool missing = status == ASSET_ERR_NOT_FOUND || status == ASSET_ERR_NO_PACK;
    if (snprintf(failure, sizeof failure, missing ? "no asset '%s': flash it" : "bad asset '%s'",
                 failed == NULL ? "?" : failed)
        < 0) {
        failure[0] = '\0';
    }
    return false;
}

/* Draws one of the scene's mesh renderers, where the scene places it. */
static void
enter_with(const r3d_instance_t* placed) {
    gfx_set_partial_clear(false);
    gfx_clear(gfx_rgb(RENDER_LAB_BACKGROUND_RGB));
    elapsed_ms = 0;
    rendered = false;
    if (!bind_meshes()) {
        return;
    }

    raster = (raster_t){
        .instances = placed,
        .instance_count = 1,
        .width = GFX_WIDTH * 100 / render_lab_scale(),
        .height = GFX_HEIGHT * 100 / render_lab_scale(),
        .clear = GFX_RGB(SKY_RGB),
        .destination = gfx_framebuffer(),
        .destination_width = GFX_WIDTH,
        .destination_height = GFX_HEIGHT,
    };
    const size_t bytes = raster_scratch_bytes(&raster);
    raster.scratch = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (raster.scratch == NULL) {
        ESP_LOGE(TAG, "no %u bytes of PSRAM for the frame: the scene stays blank", (unsigned)bytes);
    }
}

static void
scene_sponza_enter(void) {
    enter_with(&sponza_scene_atrium);
}

static void
scene_sponza_lite_enter(void) {
    enter_with(&sponza_scene_atrium_lite);
}

static void
scene_sponza_flat_enter(void) {
    enter_with(&sponza_scene_atrium_flat);
}

static void
scene_sponza_exit(void) {
    heap_caps_free(raster.scratch);
    raster.scratch = NULL;
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
    stats = raster_draw(&raster, &camera, display_shell_quarter());
#if TUNE_ENABLED
    raster_show(&raster, render_lab_view());
#endif
    rendered = true;
}

static void
scene_sponza_update(uint32_t dt_ms) {
    if (raster.scratch != NULL) {
        render(dt_ms);
    }
}

static void
scene_sponza_frame(uint32_t dt_ms, bool band_mode_active) {
    assert(!band_mode_active); /* needs_full_framebuffer keeps the app out of band mode for this scene */
    if (raster.scratch == NULL) {
        return;
    }
    if (!rendered) {
        render(dt_ms); /* no update() ran since the last frame: the first after entering */
    }
    raster_upscale(&raster);
    rendered = false;
    gfx_mark_dirty(0, 0, GFX_WIDTH, GFX_HEIGHT);
}

static const char*
sponza_status(void) {
    static char buf[48];
    if (failure[0] != '\0') {
        return failure;
    }
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
