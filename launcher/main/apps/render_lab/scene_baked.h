/*
 * scene_baked: a scene file whose baked renderers are shown one at a time.
 *
 * Every scene of a family loads the same scene file and enables one of its
 * renderers; the shell advances the camera and draws it. A family is data
 * (baked_families.h): its scene, its renderers' names and its camera.
 */
#pragma once

#include <stdint.h>

#include "baked_families.h"
#include "render_lab_scene.h"

/* What a family's scene shows: its family, and which of its bakes. */
typedef struct {
    const scene_baked_t* family;
    scene_baked_variant_t variant;
} scene_baked_shown_t;

/* `self->data` is a scene_baked_shown_t. Loads its family's scene and enables
 * that bake alone. */
void scene_baked_enter(const render_lab_scene_t* self);
void scene_baked_frame(uint32_t dt_ms);
void scene_baked_exit(void);
/* The size drawn at and the triangle count, or why the scene did not load. */
const char* scene_baked_status(void);

/* The scene that shows variant `variant_` of family `id_`, titled and keyed after it. */
#define SCENE_BAKED_SCENE(id_, variant_, name_, key_, title_suffix_, key_suffix_, label_)                              \
    [variant_] = {.name = name_ title_suffix_,                                                                         \
                  .key = key_ key_suffix_,                                                                             \
                  .family = name_,                                                                                     \
                  .variant = label_,                                                                                   \
                  .data = &id_##_shown[variant_],                                                                      \
                  .enter = scene_baked_enter,                                                                          \
                  .frame = scene_baked_frame,                                                                          \
                  .exit = scene_baked_exit,                                                                            \
                  .status = scene_baked_status}

/* Defines `id_`, the family's scenes indexed by variant: the menu family
 * `name_` with a variant row each, titled `name_` and the variant, keyed `key_`
 * and the variant. */
#define SCENE_BAKED_FAMILY(id_, family_, name_, key_)                                                                  \
    static const scene_baked_shown_t id_##_shown[SCENE_BAKED_VARIANT_COUNT] =                                          \
        {                                                                                                              \
            {&(family_), SCENE_BAKED_FULL},        {&(family_), SCENE_BAKED_FLAT},                                     \
            {&(family_), SCENE_BAKED_LITE},        {&(family_), SCENE_BAKED_FITTED},                                   \
            {&(family_), SCENE_BAKED_FITTED_FULL}, {&(family_), SCENE_BAKED_FLAT_FITTED},                              \
    };                                                                                                                 \
    const render_lab_scene_t id_[SCENE_BAKED_VARIANT_COUNT] = {                                                        \
        SCENE_BAKED_SCENE(id_, SCENE_BAKED_FULL, name_, key_, "", "", "Full"),                                         \
        SCENE_BAKED_SCENE(id_, SCENE_BAKED_FLAT, name_, key_, " Flat", "-flat", "Flat"),                               \
        SCENE_BAKED_SCENE(id_, SCENE_BAKED_LITE, name_, key_, " Lite", "-lite", "Lite"),                               \
        SCENE_BAKED_SCENE(id_, SCENE_BAKED_FITTED, name_, key_, " Fitted", "-fitted", "Fitted"),                       \
        SCENE_BAKED_SCENE(id_, SCENE_BAKED_FITTED_FULL, name_, key_, " Fitted Full", "-fitted-full", "Fitted Full"),   \
        SCENE_BAKED_SCENE(id_, SCENE_BAKED_FLAT_FITTED, name_, key_, " Flat Fitted", "-flat-fitted", "Flat Fitted"),   \
    }

/* The families render_lab shows, each a scene per variant. */
extern const render_lab_scene_t scene_sponza[SCENE_BAKED_VARIANT_COUNT];
extern const render_lab_scene_t scene_bistro[SCENE_BAKED_VARIANT_COUNT];
