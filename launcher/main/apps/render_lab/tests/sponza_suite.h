/*
 * sponza_suite: the Sponza scene a suite loads once before its tests, with
 * its pack and camera path, and lets go of after them (scene_suite.h).
 */
#pragma once

#include "apps/render_lab/sponza_content.h"
#include "apps/render_lab/tests/scene_suite.h"
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

/* Loads it, printing why when it cannot; the scene is then ENTITY_NONE.
 * Not inlined, so its failure report stays out of the suite's test chains. */
static __attribute__((noinline, unused)) sponza_suite_t
sponza_suite_load(void) {
    const scene_suite_t loaded = scene_suite_load(SPONZA_SCENE);
    sponza_suite_t s = {loaded.scene, HANDLE_NONE, NULL, loaded.arena_mark};
    if (!handle_is_none(s.scene)) {
        s.load = asset_store_load(SPONZA_SCENE);
        s.lens = camera_lens(entity_find(s.scene, name_hash(SPONZA_CAMERA)));
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
    scene_suite_release(&(scene_suite_t){s->scene, s->arena_mark});
    *s = (sponza_suite_t){ENTITY_NONE, HANDLE_NONE, NULL, 0};
}
