/*
 * scene_baked: loads a family's scene, draws its bake and keeps the
 * raster and camera settings applied; see scene_baked.h. Light is baked into
 * the mesh, so a frame is only cull, transform, clip and fill on both cores.
 * The families' scenes are defined at the end.
 */

#include "scene_baked.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

#include "core/tune.h"
#include "draw/draw.h"
#include "image/pixel/rgb565.h"
#include "render/context/render_context.h"
#include "render_lab.h"
#include "render_lab_dynres.h"
#include "render_lab_view.h"
#include "scene/camera/camera.h"
#include "scene/renderer/renderer.h"
#include "scene/scene.h"
#include "ui/render_lab_settings.h"
#include "ui/ui.h"

static const char* TAG = "scene_baked";

static const scene_baked_t* shown_family;
static entity_t loaded;
static entity_t bake;
/* Whether the bake has textures, and whether they are drawn: render_lab.textures as last applied. */
static bool texturable;
static bool textured;

/* What the panel says in place of the triangle count when the scene could not
 * load; empty when it could. */
static char failure[LAB_STATUS_LEN];

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

/* The camera the settings follow: the one the start named, else the family's. */
static entity_t
camera_entity(void) {
    return entity_find(loaded,
                       name_hash(render_lab.start_camera == NULL ? shown_family->camera : render_lab.start_camera));
}

static lab_path_t
scene_baked_path(void) {
    return (lab_path_t){.scene = loaded, .entity = handle_is_none(loaded) ? ENTITY_NONE : camera_entity()};
}

/* Draws the bake with its textures when render_lab.textures turns on, plain when it turns off; a bake
 * without textures, or loaded with the setting off, is left as it loaded. */
static void
apply_textures(void) {
    const bool on = render_lab_textures_on();
    if (!texturable || on == textured) {
        return;
    }
    textured = on;
    scene_failure_t why;
    if (!renderer_set_textured(bake, on, &why)) {
        record_failure(&why);
    }
}

static void
apply_settings(void) {
    render_lab_apply_raster_settings();
    apply_textures();
    if (!handle_is_none(loaded)) {
        (void)render_lab_apply_camera_settings(camera_entity());
    }
#if TUNE_ENABLED
    render_context_set_debug_view(render_context_main(), render_lab_debug_view());
#endif
}

entity_t
scene_baked_find(entity_t root, const scene_baked_t* family, char* failure, size_t size) {
    const entity_t bake = entity_find(root, name_hash(family->bake));
    if (handle_is_none(bake)) {
        ESP_LOGE(TAG, "the scene has no entity '%s'", family->bake);
        if (snprintf(failure, size, "no bake '%s'", family->bake) < 0) {
            failure[0] = '\0';
        }
    }
    return bake;
}

static void
scene_baked_enter(const lab_scene_t* self) {
    const scene_baked_t* family = self->data;
    draw_clear(RGB565BE(UI_BACKGROUND));
    failure[0] = '\0';
    shown_family = family;
    scene_failure_t why = {.status = SCENE_ERR_FULL}; /* what scene_load() leaves when it returns no root */
    loaded = scene_load(family->scene);
    if (scene_state(loaded) != PENDING_READY) {
        (void)scene_failure(loaded, &why);
        record_failure(&why);
        scene_release(loaded);
        loaded = ENTITY_NONE;
        return;
    }
    bake = scene_baked_find(loaded, family, failure, sizeof failure);
    texturable = !handle_is_none(bake) && renderer_has_textures(bake);
    textured = false;
    (void)camera_activate(camera_entity());
    (void)camera_set_skip_hidden(camera_entity(), family->skip_hidden);
    apply_settings();
}

static void
scene_baked_exit(void) {
    scene_release(loaded);
    loaded = ENTITY_NONE;
    bake = ENTITY_NONE;
    texturable = false;
    textured = false;
}

/* The shell has already drawn the scene into the framebuffer. */
static void
scene_baked_frame(uint32_t dt_ms) {
    (void)dt_ms;
    apply_settings();
}

/* The size drawn at and the triangle count, or why the scene did not load. */
static inline const char*
scene_baked_status(void) {
    static char buf[LAB_STATUS_LEN];
    if (failure[0] != '\0') {
        return failure;
    }
    if (snprintf(buf, sizeof buf, "%s%5d tris", render_lab_dynres_size(),
                 render_context_frame(render_context_main()).stats.triangles)
        < 0) {
        buf[0] = '\0';
    }
    return buf;
}

static void
scene_baked_controls(int width) {
    if (!texturable) {
        return;
    }
    bool on = render_lab_textures_on();
    ui_row_centre(width, UI_ROW_HEIGHT);
    if (ui_toggle(ui_next(), RENDER_LAB_TEXTURES_LABEL, &on)) {
        (void)render_lab_textures_set_on(on);
    }
}

/* A family's scene: titled and listed as `name_`, keyed `key_`, drawing `family_`'s bake. */
#define SCENE_BAKED_ENTRY(name_, key_, family_)                                                                        \
    {.name = name_,                                                                                                    \
     .key = key_,                                                                                                      \
     .family = name_,                                                                                                  \
     .data = family_,                                                                                                  \
     .enter = scene_baked_enter,                                                                                       \
     .frame = scene_baked_frame,                                                                                       \
     .exit = scene_baked_exit,                                                                                         \
     .status = LAB_SCENE_DEV(scene_baked_status),                                                                      \
     .controls = scene_baked_controls,                                                                                 \
     .generates_frames = true,                                                                                         \
     .path = scene_baked_path}

const lab_scene_t scene_sponza = SCENE_BAKED_ENTRY("Sponza", "sponza", &baked_sponza);
const lab_scene_t scene_bistro = SCENE_BAKED_ENTRY("Bistro", "bistro", &baked_bistro);
