/*
 * scene_baked: loads a family's scene, shows one of its bakes and keeps the
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

#include "console/console_words.h"
#include "core/tune.h"
#include "draw/draw.h"
#include "gfx/present/gfx_present.h"
#include "image/pixel/rgb565.h"
#include "render/context/render_context.h"
#include "render_lab.h"
#include "render_lab_dynres.h"
#include "render_lab_view.h"
#include "scene/scene.h"
#include "ui/ui.h"

static const char* TAG = "scene_baked";

static const scene_baked_t* shown_family;
static scene_t* loaded;

/* What the panel says in place of the triangle count when the scene could not
 * load; empty when it could. */
static char failure[RENDER_LAB_STATUS_LEN];

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
static scene_entity_t
camera_entity(void) {
    return scene_find(loaded,
                      name_hash(render_lab_start_camera == NULL ? shown_family->camera : render_lab_start_camera));
}

static void
apply_settings(void) {
    render_lab_apply_raster_settings();
    if (loaded != NULL) {
        (void)render_lab_apply_camera_settings(loaded, camera_entity());
    }
#if TUNE_ENABLED
    render_context_set_debug_view(render_context_main(), render_lab_debug_view());
#endif
}

/* The menu rows of a baked scene: FRAME GEN, whose warp a static scene
 * suits (development builds, where the tunable moves). */
static inline void
scene_baked_controls(int width) {
#if TUNE_ENABLED
    bool generating = render_lab_framegen_on();
    ui_row_centre(width, UI_ROW_HEIGHT);
    if (ui_toggle(ui_next(), "FRAME GEN", &generating)) {
        (void)render_lab_framegen_set_on(generating);
    }
#else
    (void)width;
#endif
}

void
scene_baked_enter(const render_lab_scene_t* self) {
    const scene_baked_shown_t* shown = self->data;
    const scene_baked_t* family = shown->family;
    draw_clear(RGB565BE(UI_BACKGROUND));
    failure[0] = '\0';
    shown_family = family;
    scene_failure_t why;
    loaded = scene_load(family->scene, &why);
    if (loaded == NULL) {
        record_failure(&why);
        return;
    }
    for (int i = 0; i < (int)SCENE_BAKED_VARIANT_COUNT; i++) {
        const scene_entity_t bake = scene_find(loaded, name_hash(family->bakes[i]));
        if (bake == SCENE_ENTITY_NONE) {
            ESP_LOGE(TAG, "the scene has no entity '%s'", family->bakes[i]);
            if (snprintf(failure, sizeof failure, "no bake '%s'", family->bakes[i]) < 0) {
                failure[0] = '\0';
            }
            continue;
        }
        scene_entity_set_enabled(loaded, bake, i == (int)shown->variant);
    }
    (void)scene_activate(loaded,
                         render_lab_start_camera == NULL ? SCENE_CAMERA_FIRST : name_hash(render_lab_start_camera));
    apply_settings();
}

void
scene_baked_exit(void) {
    scene_unload(loaded);
    loaded = NULL;
}

/* The shell has already drawn the scene into the framebuffer. */
void
scene_baked_frame(uint32_t dt_ms) {
    (void)dt_ms;
    apply_settings();
}

/* The size drawn at and the triangle count, or why the scene did not load. */
static inline const char*
scene_baked_status(void) {
    static char buf[RENDER_LAB_STATUS_LEN];
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

/* "render pose <ms>" holds the camera at that time along its path, so a view
 * can be compared at several sizes; "render pose play" lets it move again. */
static inline bool
scene_baked_console(const char* args, console_reply_fn reply) {
    const char* rest;
    if (!console_word_match(args, "pose", &rest)) {
        return false;
    }
    const scene_entity_t camera = loaded == NULL ? SCENE_ENTITY_NONE : camera_entity();
    char* end;
    const long t_ms = strtol(rest, &end, 10);
    const bool play = strcmp(rest, "play") == 0;
    if (camera == SCENE_ENTITY_NONE) {
        reply("RENDER_ERR no camera path");
    } else if (play) {
        (void)scene_animator_set_paused(loaded, camera, SCENE_ANIMATOR_CURRENT, false);
        reply("RENDER pose=play");
        reply("RENDER_END");
    } else if (end == rest || *end != '\0' || t_ms < 0
               || !scene_animator_seek(loaded, camera, SCENE_ANIMATOR_CURRENT, (uint32_t)t_ms)
               || !scene_animator_set_paused(loaded, camera, SCENE_ANIMATOR_CURRENT, true)) {
        reply("RENDER_ERR usage: render pose <ms> | render pose play");
    } else {
        char line[sizeof("RENDER pose=-2147483648")];
        (void)snprintf(line, sizeof line, "RENDER pose=%ld", t_ms);
        reply(line);
        reply("RENDER_END");
    }
    return true;
}

SCENE_BAKED_FAMILY(scene_sponza, sponza_shown, &baked_sponza, "Sponza", "sponza");
SCENE_BAKED_FAMILY(scene_bistro, bistro_shown, &baked_bistro, "Bistro", "bistro");
