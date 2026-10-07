/*
 * sponza_suite: the Sponza scene a suite loads once before its tests, with
 * its pack and camera path, and lets go of after them.
 */
#pragma once

#include <stdio.h>

#include "apps/render_lab/sponza_content.h"
#include "asset/asset_store.h"
#include "scene/scene.h"

typedef struct {
    scene_t* scene;
    const asset_pack_t* pack;
    const r3d_scene_camera_t* path;
} sponza_suite_t;

/* Loads it, printing why when it cannot; every field is then NULL. */
static inline sponza_suite_t
sponza_suite_load(void) {
    scene_failure_t why;
    sponza_suite_t s = {scene_load(SPONZA_SCENE, &why), NULL, NULL};
    if (s.scene == NULL) {
        printf("scene %s: status %d, asset %s, about '%s'\n", SPONZA_SCENE, (int)why.status,
               asset_status_text(why.asset), why.what);
    } else {
        s.pack = asset_store_pack(SPONZA_SCENE);
        s.path = scene_camera_lens(s.scene, NULL);
    }
    return s;
}

static inline void
sponza_suite_release(sponza_suite_t* s) {
    if (s->pack != NULL) {
        asset_store_release(SPONZA_SCENE);
    }
    scene_unload(s->scene);
    *s = (sponza_suite_t){NULL, NULL, NULL};
}
