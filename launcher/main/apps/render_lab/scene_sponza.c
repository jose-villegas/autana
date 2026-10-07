/*
 * scene_sponza: Crytek Sponza flown through on a looping camera path.
 *
 * The scene manager owns the frame: this loads the scene, shows one of its
 * bakes (sponza_bakes) and draws the HUD, while the shell advances the camera
 * and draws it. Light is baked into the mesh, so a frame is only cull,
 * transform, clip and fill on both cores. One scene per bake shares this code.
 */

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_log.h"

#include "gfx/gfx.h"
#include "render/context/render_context.h"
#include "render_lab.h"
#include "render_lab_scene.h"
#include "render_lab_view.h"
#include "scene/scene.h"
#include "sponza_content.h"
#include "util/runtime/tune.h"

static const char* TAG = "sponza";

/* The budget last handed to the scene; -1 for none yet. */
static int applied_budget_ms = -1;

static void
apply_budget(void) {
    const int budget_ms = render_lab_budget_ms();
    if (budget_ms == applied_budget_ms) {
        return;
    }
    applied_budget_ms = budget_ms;
    if (budget_ms == 0) {
        render_context_set_dynamic_resolution(render_context_main(), NULL, NULL, 0);
        return;
    }
    const resolution_config_t config =
        resolution_config(sponza_ladder, SPONZA_LADDER_STEPS, SPONZA_LADDER_RECOVERY, budget_ms * 1000);
    render_context_set_dynamic_resolution(render_context_main(), &config, &sponza_ladder_model, SPONZA_LADDER_HALF);
}

static scene_t* sponza;

/* What the panel says in place of the triangle count when the scene could not
 * load; empty when it could. */
static char failure[48];

static void
record_failure(const scene_failure_t* why) {
    const char* what = why->what;
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

/* Loads the scene and shows sponza_bakes[shown]. */
static void
enter_with(sponza_bake_t shown) {
    gfx_set_partial_clear(false);
    gfx_clear(gfx_rgb(RENDER_LAB_BACKGROUND_RGB));
    failure[0] = '\0';
    scene_failure_t why;
    sponza = scene_load(SPONZA_SCENE, &why);
    if (sponza == NULL) {
        record_failure(&why);
        return;
    }
    for (int i = 0; i < (int)SPONZA_BAKE_COUNT; i++) {
        const scene_entity_t bake = scene_find(sponza, sponza_bakes[i]);
        if (bake == SCENE_ENTITY_NONE) {
            ESP_LOGE(TAG, "the scene has no entity '%s'", sponza_bakes[i]);
            if (snprintf(failure, sizeof failure, "no bake '%s'", sponza_bakes[i]) < 0) {
                failure[0] = '\0';
            }
            continue;
        }
        scene_entity_set_enabled(sponza, bake, i == (int)shown);
    }
    (void)scene_activate(sponza, NULL);
    render_context_set_scale(render_context_main(), 10000 / render_lab_scale());
    applied_budget_ms = -1;
    apply_budget();
#if TUNE_ENABLED
    render_context_set_debug_view(render_context_main(), render_lab_view());
#endif
}

static void
scene_sponza_enter(void) {
    enter_with(SPONZA_BAKE_FULL);
}

static void
scene_sponza_lite_enter(void) {
    enter_with(SPONZA_BAKE_LITE);
}

static void
scene_sponza_flat_enter(void) {
    enter_with(SPONZA_BAKE_FLAT);
}

static void
scene_sponza_fitted_enter(void) {
    enter_with(SPONZA_BAKE_FITTED);
}

static void
scene_sponza_fitted_full_enter(void) {
    enter_with(SPONZA_BAKE_FITTED_FULL);
}

static void
scene_sponza_flat_fitted_enter(void) {
    enter_with(SPONZA_BAKE_FLAT_FITTED);
}

static void
scene_sponza_exit(void) {
    render_context_set_dynamic_resolution(render_context_main(), NULL, NULL, 0);
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
    apply_budget();
#if TUNE_ENABLED
    render_context_set_debug_view(render_context_main(), render_lab_view());
#endif
}

static const char*
sponza_status(void) {
    static char buf[48];
    if (failure[0] != '\0') {
        return failure;
    }
    /* Under a budget, the size the frame drew at, so a step shows on the panel. */
    const render_context_frame_t r = render_context_frame(render_context_main());
    const int wrote =
        r.step < 0 ? snprintf(buf, sizeof buf, "%5d tris", render_context_frame(render_context_main()).stats.triangles)
                   : snprintf(buf, sizeof buf, "%dx%d %5d tris", r.width, r.height,
                              render_context_frame(render_context_main()).stats.triangles);
    if (wrote < 0) {
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

const render_lab_scene_t scene_sponza_flat_fitted = {
    .name = "Sponza Flat Fitted",
    .key = "sponza-flat-fitted",
    .enter = scene_sponza_flat_fitted_enter,
    .frame = scene_sponza_frame,
    .update = NULL,
    .frame_band = NULL,
    .exit = scene_sponza_exit,
    .invalidate = scene_sponza_invalidate,
    .status = sponza_status,
    .needs_full_framebuffer = true,
    .shows_view_modes = true,
};
