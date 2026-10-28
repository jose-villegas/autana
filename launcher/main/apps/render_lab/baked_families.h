/*
 * baked_families: the scene files render_lab shows one bake at a time, each a
 * family of scenes (scene_baked.h).
 */
#pragma once

#include "apps/render_lab/scene_baked_variant.h"

typedef struct {
    const char* scene;        /* the scene's id, and its pack's name */
    const char* const* bakes; /* the renderer of each variant, by scene_baked_variant_t */
    const char* camera;       /* followed when the start names no camera */
} scene_baked_t;

/* An open street with a far plane, walked and flown over on a loop. */
#define BISTRO_SCENE  "bistro"
#define BISTRO_CAMERA "camera"

extern const scene_baked_t baked_sponza;
extern const scene_baked_t baked_bistro;

#define BAKED_FAMILY_COUNT 2
extern const scene_baked_t* const baked_families[BAKED_FAMILY_COUNT];
