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

/* One variant's rows of SCENE_BAKED_FAMILY, with its family's fields as `f_`:
 * (shown table, family, name, key prefix). */
#define SCENE_BAKED_FIELD_SHOWN(shown_, family_, name_, key_prefix_)  shown_
#define SCENE_BAKED_FIELD_FAMILY(shown_, family_, name_, key_prefix_) family_
#define SCENE_BAKED_FIELD_NAME(shown_, family_, name_, key_prefix_)   name_
#define SCENE_BAKED_FIELD_KEY(shown_, family_, name_, key_prefix_)    key_prefix_
#define SCENE_BAKED_SHOWN(f_, id_, title_, key_, label_)              {SCENE_BAKED_FIELD_FAMILY f_, SCENE_BAKED_##id_},
#define SCENE_BAKED_SCENE(f_, id_, title_, key_, label_)                                                               \
    [SCENE_BAKED_##id_] = {.name = SCENE_BAKED_FIELD_NAME f_ title_,                                                   \
                           .key = SCENE_BAKED_FIELD_KEY f_ key_,                                                       \
                           .family = SCENE_BAKED_FIELD_NAME f_,                                                        \
                           .variant = label_,                                                                          \
                           .data = &SCENE_BAKED_FIELD_SHOWN f_[SCENE_BAKED_##id_],                                     \
                           .enter = scene_baked_enter,                                                                 \
                           .frame = scene_baked_frame,                                                                 \
                           .exit = scene_baked_exit,                                                                   \
                           .status = RENDER_LAB_SCENE_DEV(scene_baked_status),                                         \
                           .controls = RENDER_LAB_SCENE_DEV(scene_baked_controls),                                     \
                           .console = RENDER_LAB_SCENE_DEV(scene_baked_console)},

/* Defines `id`, family `family_`'s scenes indexed by variant: the menu family
 * `name_`, each titled `name_` and its variant and keyed `key_prefix_` and its
 * variant; `shown_` names the table of what each shows. */
#define SCENE_BAKED_FAMILY(id, shown_, family_, name_, key_prefix_)                                                    \
    static const scene_baked_shown_t shown_[SCENE_BAKED_VARIANT_COUNT] = {                                             \
        SCENE_BAKED_VARIANTS(SCENE_BAKED_SHOWN, (shown_, family_, name_, key_prefix_))};                               \
    const render_lab_scene_t id[SCENE_BAKED_VARIANT_COUNT] = {                                                         \
        SCENE_BAKED_VARIANTS(SCENE_BAKED_SCENE, (shown_, family_, name_, key_prefix_))}

/* The families render_lab shows, each a scene per variant. */
extern const render_lab_scene_t scene_sponza[SCENE_BAKED_VARIANT_COUNT];
extern const render_lab_scene_t scene_bistro[SCENE_BAKED_VARIANT_COUNT];
