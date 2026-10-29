/*
 * baked_families: the scene files render_lab shows one bake at a time, each a
 * family of scenes (demo_baked.h).
 */
#pragma once

#include "apps/render_lab/demo_baked_variant.h"

typedef struct {
    const char* scene;        /* the scene's id, and its pack's name */
    const char* const* bakes; /* the renderer of each variant, by demo_baked_variant_t */
    const char* camera;       /* followed when the start names no camera */
} demo_baked_t;

/* An open street with a far plane, walked and flown over on a loop. */
#define BISTRO_SCENE  "bistro"
#define BISTRO_CAMERA "camera"

extern const demo_baked_t baked_sponza;
extern const demo_baked_t baked_bistro;

#define BAKED_FAMILY_COUNT 2
extern const demo_baked_t* const baked_families[BAKED_FAMILY_COUNT];
