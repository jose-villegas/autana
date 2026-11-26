/*
 * scene_suite: a scene a suite loads for its tests and lets go of after them.
 * Like an app's visit, the suite gives back the app arena the scene took when
 * it lets go.
 */
#pragma once

#include "app/app_arena.h"
#include "lab/lab_scene.h"
#include "scene/scene.h"

typedef struct {
    entity_t scene;
    size_t arena_mark;
} scene_suite_t;

/* Loads `key`, logging why when it cannot (lab_scene_load); the scene is then
 * ENTITY_NONE. Not inlined, so the load stays out of the suite's test chains. */
static __attribute__((noinline, unused)) scene_suite_t
scene_suite_load(const char* key) {
    char failure[LAB_STATUS_LEN];
    const size_t mark = app_arena_mark();
    return (scene_suite_t){lab_scene_load("scene_suite", failure, sizeof failure, key, key), mark};
}

static inline void
scene_suite_release(scene_suite_t* s) {
    scene_release(s->scene);
    app_arena_rewind(s->arena_mark);
    *s = (scene_suite_t){ENTITY_NONE, app_arena_mark()}; /* a second release gives back nothing */
}
