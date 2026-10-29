/*
 * demo_baked: a scene file whose baked renderers are shown one at a time.
 *
 * Every scene of a family loads the same scene file and enables one of its
 * renderers; the shell advances the camera and draws it. A family is data
 * (baked_families.h): its scene, its renderers' names and its camera.
 */
#pragma once

#include <stdint.h>

#include "baked_families.h"
#include "render_lab_demo.h"

/* What a family's scene shows: its family, and which of its bakes. */
typedef struct {
    const demo_baked_t* family;
    demo_baked_variant_t variant;
} demo_baked_shown_t;

/* `self->data` is a demo_baked_shown_t. Loads its family's scene and enables
 * that bake alone. */
void demo_baked_enter(const render_lab_demo_t* self);
void demo_baked_frame(uint32_t dt_ms);
void demo_baked_exit(void);
/* The size drawn at and the triangle count, or why the scene did not load. */
const char* demo_baked_status(void);

/* One variant's rows of DEMO_BAKED_FAMILY, with its family's fields as `f_`:
 * (shown table, family, name, key prefix). */
#define DEMO_BAKED_FIELD_SHOWN(shown_, family_, name_, key_prefix_)  shown_
#define DEMO_BAKED_FIELD_FAMILY(shown_, family_, name_, key_prefix_) family_
#define DEMO_BAKED_FIELD_NAME(shown_, family_, name_, key_prefix_)   name_
#define DEMO_BAKED_FIELD_KEY(shown_, family_, name_, key_prefix_)    key_prefix_
#define DEMO_BAKED_SHOWN(f_, id_, title_, key_, label_)              {DEMO_BAKED_FIELD_FAMILY f_, DEMO_BAKED_##id_},
#define DEMO_BAKED_SCENE(f_, id_, title_, key_, label_)                                                                \
    [DEMO_BAKED_##id_] = {.name = DEMO_BAKED_FIELD_NAME f_ title_,                                                     \
                          .key = DEMO_BAKED_FIELD_KEY f_ key_,                                                         \
                          .family = DEMO_BAKED_FIELD_NAME f_,                                                          \
                          .variant = label_,                                                                           \
                          .data = &DEMO_BAKED_FIELD_SHOWN f_[DEMO_BAKED_##id_],                                        \
                          .enter = demo_baked_enter,                                                                   \
                          .frame = demo_baked_frame,                                                                   \
                          .exit = demo_baked_exit,                                                                     \
                          .status = demo_baked_status},

/* Defines `id`, family `family_`'s scenes indexed by variant: the menu family
 * `name_`, each titled `name_` and its variant and keyed `key_prefix_` and its
 * variant; `shown_` names the table of what each shows. */
#define DEMO_BAKED_FAMILY(id, shown_, family_, name_, key_prefix_)                                                     \
    static const demo_baked_shown_t shown_[DEMO_BAKED_VARIANT_COUNT] = {                                               \
        DEMO_BAKED_VARIANTS(DEMO_BAKED_SHOWN, (shown_, family_, name_, key_prefix_))};                                 \
    const render_lab_demo_t id[DEMO_BAKED_VARIANT_COUNT] = {                                                           \
        DEMO_BAKED_VARIANTS(DEMO_BAKED_SCENE, (shown_, family_, name_, key_prefix_))}

/* The families render_lab shows, each a scene per variant. */
extern const render_lab_demo_t demo_sponza[DEMO_BAKED_VARIANT_COUNT];
extern const render_lab_demo_t demo_bistro[DEMO_BAKED_VARIANT_COUNT];
