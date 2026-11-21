/*
 * baked_families: the scene files render_lab shows by their baked renderer,
 * each a family of scenes (scene_baked.h).
 */
#pragma once

#include <stdbool.h>

typedef struct {
    const char* scene;  /* the scene's id, and its pack's name */
    const char* bake;   /* the entity of the scene's fitted renderer, found at load */
    const char* camera; /* followed when the start names no camera */
    bool skip_hidden;   /* its camera skips clusters behind drawn depth: much of it hides */
} scene_baked_t;

/* An open street with a far plane, walked and flown over on a loop. */
#define BISTRO_SCENE  "bistro"
#define BISTRO_CAMERA "camera"

extern const scene_baked_t baked_sponza;
extern const scene_baked_t baked_bistro;

#define BAKED_FAMILY_COUNT 2
extern const scene_baked_t* const baked_families[BAKED_FAMILY_COUNT];
