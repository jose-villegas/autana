/*
 * scene_baked: a scene file whose baked renderers are shown one at a time.
 *
 * Every scene of a family loads the same scene file and enables one of its
 * renderers; the shell advances the camera and draws it. A family is data: its
 * scene, the renderers it switches between and the camera it starts on.
 */
#pragma once

#include <stdint.h>

typedef struct {
    const char* scene;        /* the scene's id, and its pack's name */
    const char* const* bakes; /* the renderers one scene each shows */
    int bake_count;
    const char* const* cameras; /* [0] is the camera a start with none named follows */
} scene_baked_t;

/* Loads the family's scene and enables bakes[shown] alone. */
void scene_baked_enter(const scene_baked_t* family, int shown);
void scene_baked_frame(uint32_t dt_ms);
void scene_baked_exit(void);
/* The size drawn at and the triangle count, or why the scene did not load. */
const char* scene_baked_status(void);

/* Defines `scene_`, the scene that shows the family's bake `shown_`: its menu
 * family, HUD title, key and variant, with the shared frame, exit and status. */
#define SCENE_BAKED(scene_, family_, shown_, family_name_, title_, key_, variant_)                                     \
    static void scene_##_enter(void) { scene_baked_enter(&(family_), (shown_)); }                                      \
    const render_lab_scene_t scene_ = {.name = (title_),                                                               \
                                       .key = (key_),                                                                  \
                                       .family = (family_name_),                                                       \
                                       .variant = (variant_),                                                          \
                                       .enter = scene_##_enter,                                                        \
                                       .frame = scene_baked_frame,                                                     \
                                       .exit = scene_baked_exit,                                                       \
                                       .status = scene_baked_status}
