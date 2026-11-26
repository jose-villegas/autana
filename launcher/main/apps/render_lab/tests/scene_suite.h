/*
 * scene_suite: a scene a suite loads for its tests and lets go of after them.
 * Like an app's visit, the suite gives back the app arena the scene took when
 * it lets go.
 */
#pragma once

#include <stdio.h>

#include "app/app_arena.h"
#include "asset/asset_pack.h"
#include "scene/scene.h"

typedef struct {
    entity_t scene;
    size_t arena_mark;
} scene_suite_t;

/* Loads `key`, printing why when it cannot; the scene is then ENTITY_NONE.
 * Not inlined, so its failure report stays out of the suite's test chains. */
static __attribute__((noinline, unused)) scene_suite_t
scene_suite_load(const char* key) {
    scene_failure_t why = {.status = SCENE_ERR_FULL}; /* what scene_load() leaves when it returns no root */
    scene_suite_t s = {.arena_mark = app_arena_mark()};
    s.scene = scene_load(key);
    if (scene_state(s.scene) != PENDING_READY) {
        (void)scene_failure(s.scene, &why);
        printf("scene %s: status %d, asset %s, about '%s'\n", key, (int)why.status, asset_status_text(why.asset),
               why.what);
        scene_release(s.scene);
        s.scene = ENTITY_NONE;
    }
    return s;
}

static inline void
scene_suite_release(scene_suite_t* s) {
    scene_release(s->scene);
    app_arena_rewind(s->arena_mark);
    *s = (scene_suite_t){ENTITY_NONE, 0};
}
