/*
 * scene_sponza: Crytek Sponza flown through on a looping camera path.
 *
 * The scene manager owns the frame: this loads the scene, shows one of its
 * five bakes and draws the HUD, while the shell advances the camera and
 * draws it. Light is baked into the mesh, so a frame is only cull, transform,
 * clip and fill on both cores. Five scenes share this code, one per bake:
 * full, lite, flat, fitted and fitted full.
 */

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_log.h"

#include "gfx/gfx.h"
#include "meshes/sponza_scene_generated.h"
#include "render_lab.h"
#include "render_lab_scene.h"
#include "render_lab_view.h"
#include "scene/scene.h"
#include "util/tune.h"

static const char* TAG = "sponza";

static scene_t* sponza;

/* What the panel says in place of the triangle count when the scene could not
 * load; empty when it could. */
static char failure[48];

static void
record_failure(const scene_failure_t* why) {
    const char* what = why->what == NULL ? "?" : why->what;
    ESP_LOGE(TAG, "scene '%s' did not load (%d, asset %s); it stays blank", what, (int)why->status,
             asset_status_text(why->asset));
    const bool missing = why->asset == ASSET_ERR_NOT_FOUND || why->asset == ASSET_ERR_NO_PACK;
    const char* shown = "bad asset '%s'";
    switch (why->status) {
        case SCENE_ERR_MEMORY: shown = "no memory for '%s'"; break;
        case SCENE_ERR_FULL: shown = "too many scenes for '%s'"; break;
        case SCENE_ERR_ASSET: shown = missing ? "no asset '%s': flash it" : "bad asset '%s'"; break;
        case SCENE_ERR_UNKNOWN:
        case SCENE_OK: shown = "no scene '%s'"; break;
    }
    if (snprintf(failure, sizeof failure, shown, what) < 0) {
        failure[0] = '\0';
    }
}

/* Loads the scene and shows `variant`, the one of its five bakes to draw. */
static void
enter_with(scene_entity_t variant) {
    gfx_set_partial_clear(false);
    gfx_clear(gfx_rgb(RENDER_LAB_BACKGROUND_RGB));
    failure[0] = '\0';
    scene_failure_t why;
    sponza = scene_load("sponza", &why);
    if (sponza == NULL) {
        record_failure(&why);
        return;
    }
    const scene_entity_t bakes[] = {SPONZA_SCENE_ATRIUM, SPONZA_SCENE_ATRIUM_FLAT, SPONZA_SCENE_ATRIUM_LITE,
                                    SPONZA_SCENE_ATRIUM_FITTED, SPONZA_SCENE_ATRIUM_FITTED_FULL};
    for (size_t i = 0; i < sizeof bakes / sizeof bakes[0]; i++) {
        scene_entity_set_enabled(sponza, bakes[i], bakes[i] == variant);
    }
    (void)scene_activate(sponza, NULL);
    scene_set_render_scale(10000 / render_lab_scale());
#if TUNE_ENABLED
    scene_set_debug_view(render_lab_view());
#endif
}

static void
scene_sponza_enter(void) {
    enter_with(SPONZA_SCENE_ATRIUM);
}

static void
scene_sponza_lite_enter(void) {
    enter_with(SPONZA_SCENE_ATRIUM_LITE);
}

static void
scene_sponza_flat_enter(void) {
    enter_with(SPONZA_SCENE_ATRIUM_FLAT);
}

static void
scene_sponza_fitted_enter(void) {
    enter_with(SPONZA_SCENE_ATRIUM_FITTED);
}

static void
scene_sponza_fitted_full_enter(void) {
    enter_with(SPONZA_SCENE_ATRIUM_FITTED_FULL);
}

static void
scene_sponza_exit(void) {
    scene_unload(sponza);
    sponza = NULL;
}

/* Every frame already redraws the whole screen. */
static void
scene_sponza_invalidate(void) {}

/* The shell has already drawn the scene into the framebuffer. */
static void
scene_sponza_frame(uint32_t dt_ms, bool band_mode_active) {
    (void)dt_ms;
    assert(!band_mode_active); /* needs_full_framebuffer keeps the app out of band mode for this scene */
#if TUNE_ENABLED
    scene_set_debug_view(render_lab_view());
#endif
}

static const char*
sponza_status(void) {
    static char buf[48];
    if (failure[0] != '\0') {
        return failure;
    }
    if (snprintf(buf, sizeof buf, "%5d tris", scene_stats().triangles) < 0) {
        buf[0] = '\0';
    }
    return buf;
}

const render_lab_scene_t scene_sponza = {
    .name = "Sponza",
    .key = "sponza",
    .enter = scene_sponza_enter,
    .frame = scene_sponza_frame,
    .update = NULL,
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
    .update = NULL,
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
    .update = NULL,
    .frame_band = NULL,
    .exit = scene_sponza_exit,
    .invalidate = scene_sponza_invalidate,
    .status = sponza_status,
    .needs_full_framebuffer = true,
    .shows_view_modes = true,
};

const render_lab_scene_t scene_sponza_fitted = {
    .name = "Sponza Fitted",
    .key = "sponza-fitted",
    .enter = scene_sponza_fitted_enter,
    .frame = scene_sponza_frame,
    .update = NULL,
    .frame_band = NULL,
    .exit = scene_sponza_exit,
    .invalidate = scene_sponza_invalidate,
    .status = sponza_status,
    .needs_full_framebuffer = true,
    .shows_view_modes = true,
};

const render_lab_scene_t scene_sponza_fitted_full = {
    .name = "Sponza Fitted Full",
    .key = "sponza-fitted-full",
    .enter = scene_sponza_fitted_full_enter,
    .frame = scene_sponza_frame,
    .update = NULL,
    .frame_band = NULL,
    .exit = scene_sponza_exit,
    .invalidate = scene_sponza_invalidate,
    .status = sponza_status,
    .needs_full_framebuffer = true,
    .shows_view_modes = true,
};
