/*
 * sponza_suite: the Sponza scene a suite loads once before its tests, with
 * its pack and camera path, and lets go of after them. Like an app's visit,
 * the suite gives back the app arena its scenes took when it lets go.
 */
#pragma once

#include <stdio.h>

#include "app/app_arena.h"
#include "apps/render_lab/sponza_content.h"
#include "asset/asset_store.h"
#include "scene/camera/camera.h"
#include "scene/scene.h"
#include "scene/transform/transform.h"

typedef struct {
    entity_t scene;
    handle_t load; /* the pack: asset_store_loaded(load) */
    const lens_t* lens;
    size_t arena_mark;
} sponza_suite_t;

/* Loads it, printing why when it cannot; the scene is then ENTITY_NONE. */
static inline sponza_suite_t
sponza_suite_load(void) {
    scene_failure_t why = {.status = SCENE_ERR_FULL}; /* what scene_load() leaves when it returns no root */
    const size_t mark = app_arena_mark();
    sponza_suite_t s = {scene_load(SPONZA_SCENE), HANDLE_NONE, NULL, mark};
    if (scene_state(s.scene) != PENDING_READY) {
        (void)scene_failure(s.scene, &why);
        printf("scene %s: status %d, asset %s, about '%s'\n", SPONZA_SCENE, (int)why.status,
               asset_status_text(why.asset), why.what);
        scene_release(s.scene);
        s.scene = ENTITY_NONE;
    } else {
        s.load = asset_store_load(SPONZA_SCENE);
        s.lens = camera_lens(camera_first(s.scene));
    }
    return s;
}

/* The view through `camera` now, from its world pose. Not inlined, so the
 * pose copy stays out of the deep draw chains of the tests that call it. */
static __attribute__((noinline, unused)) render_view_t
sponza_suite_view(const lens_t* lens, entity_t scene, entity_t camera, viewport_t viewport) {
    const transformf_t pose = transform_world(camera);
    return lens_view(lens, &pose, viewport);
}

static inline void
sponza_suite_release(sponza_suite_t* s) {
    asset_store_release(s->load);
    scene_release(s->scene);
    app_arena_rewind(s->arena_mark);
    *s = (sponza_suite_t){ENTITY_NONE, HANDLE_NONE, NULL, 0};
}
