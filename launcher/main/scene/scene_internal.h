/*
 * scene_internal: the one scene allocation's layout, shared by scene.c (load,
 * entities) and scene_draw.c (the active camera and the draw). Not for apps.
 */
#pragma once

#include "scene/scene.h"
#include "scene/scene_shell.h"

#define SCENE_FLAG_ENABLED 1U
#define SCENE_FLAG_DIRTY   2U

/* A mesh view and the placement built from its entity's transform. */
typedef struct {
    r3d_lit_mesh_t mesh;
    r3d_placement_t placement;
    scene_entity_t entity;
    bool placed; /* false while the transform is the identity: drawn as it is */
} scene_renderer_t;

typedef struct {
    r3d_scene_camera_t lens;
    scene_entity_t entity;
    int render_scale_percent;
    uint16_t clear; /* panel pixel format */
} scene_camera_t;

struct scene {
    const scene_def_t* def;
    bool holds_bundle; /* loaded by scene_load(): a use of bundle def->name */
    uint32_t elapsed_ms;
    scene_transform_t* transforms; /* one per entity */
    uint8_t* flags;                /* one per entity: SCENE_FLAG_* */
    scene_renderer_t* renderers;
    scene_camera_t* cameras;
    r3d_instance_t* instances; /* one per renderer: what the raster draws this frame */
};

#if !defined(ESP_PLATFORM) || CONFIG_LAUNCHER_DEVELOPMENT
/* A suite sets it to make the next scene allocation fail once, so an out of
 * memory load is shown without emptying the heap. */
extern bool scene_fail_next_allocation;
#endif

/* The loaded scenes, in load order. */
int scene_loaded_count(void);
scene_t* scene_loaded_at(int index);

/* A scene is going away: the draw drops it if its camera is active. */
void scene_draw_forget(const scene_t* scene);
void scene_draw_release(void);

/* True when drawing the mesh as it is gives the same picture. */
bool scene_transform_is_identity(const scene_transform_t* transform);
