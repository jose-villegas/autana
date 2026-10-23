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
#include "scene/scene.h"

typedef struct {
    scene_t* scene;
    const asset_pack_t* pack;
    const camera_t* lens;
    size_t arena_mark;
} sponza_suite_t;

/* Loads it, printing why when it cannot; every field is then NULL. */
static inline sponza_suite_t
sponza_suite_load(void) {
    scene_failure_t why;
    const size_t mark = app_arena_mark();
    sponza_suite_t s = {scene_load(SPONZA_SCENE, &why), NULL, NULL, mark};
    if (s.scene == NULL) {
        printf("scene %s: status %d, asset %s, about '%s'\n", SPONZA_SCENE, (int)why.status,
               asset_status_text(why.asset), why.what);
    } else {
        s.pack = asset_store_pack(SPONZA_SCENE);
        s.lens = scene_camera_lens(s.scene, NULL);
    }
    return s;
}

static inline void
sponza_suite_release(sponza_suite_t* s) {
    if (s->pack != NULL) {
        asset_store_release(SPONZA_SCENE);
    }
    scene_unload(s->scene);
    app_arena_rewind(s->arena_mark);
    *s = (sponza_suite_t){NULL, NULL, NULL, 0};
}
