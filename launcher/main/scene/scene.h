/*
 * scene: the engine's scene manager. An app loads scenes by name and
 * activates one camera; the shell advances every loaded scene and draws
 * through the active camera each frame, so an app never calls draw.
 *
 * Storage is data-oriented. A scene is one allocation of packed component
 * arrays indexed by a small entity id: a transform per entity, a renderer
 * (mesh view plus entity id) and a camera (lens, path plus entity id) only
 * for the entities that have one. Names are looked up at setup time only.
 * Lights are baked offline and are not carried.
 *
 * Exactly one camera is active engine-wide, and it draws the enabled
 * renderers of its own scene. Scenes belong to the app running when they were
 * loaded: the shell unloads whatever is left when it exits.
 *
 * The table a scene is loaded from (scene_def_t) is generated const data,
 * registered by name; docs/render/Scene-Manager.md says how a frame goes.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "asset/asset_pack.h"
#include "render/r3d_instance.h"
#include "render/r3d_scene.h"
#include "render/raster.h"

typedef struct scene scene_t;

/* An index into one scene's component arrays. */
typedef uint16_t scene_entity_t;
#define SCENE_ENTITY_NONE ((scene_entity_t)0xFFFFU)

/* Where an entity stands: the placement the raster reads. */
typedef r3d_placement_t scene_transform_t;

typedef struct {
    scene_entity_t entity;
    const char* asset; /* the lit mesh's id in the asset pack */
} scene_renderer_def_t;

typedef struct {
    scene_entity_t entity;
    r3d_scene_camera_t lens; /* its placement NULL: a scene points it at the entity's transform */
    uint32_t clear_rgb;      /* 0xRRGGBB: what no mesh covers, 0 black */
} scene_camera_def_t;

/* What scene_load() instantiates: array lengths and contents, all const. */
typedef struct {
    const char* name;
    uint16_t entity_count, renderer_count, camera_count;
    const char* const* entity_names;       /* entity_count */
    const scene_transform_t* transforms;   /* entity_count */
    const scene_renderer_def_t* renderers; /* renderer_count */
    const scene_camera_def_t* cameras;     /* camera_count */
} scene_def_t;

/* Makes a def loadable by name; runs before main(), like APP_REGISTER(). */
void scene_register(const scene_def_t* def);
#define SCENE_REGISTER(def)                                                                                            \
    __attribute__((constructor)) static void def##_register(void) { scene_register(&(def)); }

typedef enum {
    SCENE_OK = 0,
    SCENE_ERR_UNKNOWN, /* no scene has that name */
    SCENE_ERR_FULL,    /* as many scenes are loaded as the manager holds */
    SCENE_ERR_MEMORY,  /* the scene's block could not be allocated */
    SCENE_ERR_ASSET,   /* a mesh did not open: `asset` says why */
} scene_status_t;

/* What a load reports: its status, the pack's status when a mesh failed, and
 * the scene or mesh id it was about. */
typedef struct {
    scene_status_t status;
    asset_status_t asset;
    const char* what;
} scene_failure_t;

/* Loads the scene `name` beside any already loaded, its meshes opened from
 * `pack`, or for scene_load() from the asset bundle named after the scene,
 * held until the scene unloads. NULL on failure, and `why`, which may be
 * NULL, says why; a success sets it to SCENE_OK. */
scene_t* scene_load(const char* name, scene_failure_t* why);
scene_t* scene_load_from(const asset_pack_t* pack, const char* name, scene_failure_t* why);

/* Frees the scene. If its camera was the active one, nothing is drawn. */
void scene_unload(scene_t* scene);

/* The entity named `name`, or SCENE_ENTITY_NONE. */
scene_entity_t scene_find(const scene_t* scene, const char* name);

const scene_transform_t* scene_entity_transform(const scene_t* scene, scene_entity_t entity);
void scene_entity_set_transform(scene_t* scene, scene_entity_t entity, const scene_transform_t* transform);
bool scene_entity_enabled(const scene_t* scene, scene_entity_t entity);

/* A disabled entity's renderer is not drawn; every entity starts enabled. */
void scene_entity_set_enabled(scene_t* scene, scene_entity_t entity, bool enabled);

/* Makes the scene's camera `camera` (NULL: its first) the active one. False
 * when the scene has no such camera. */
bool scene_activate(scene_t* scene, const char* camera);

/* No camera is active, so nothing is drawn, until the next scene_activate(). */
void scene_deactivate(void);

/* While paused nothing advances or draws, as if no camera were active, and
 * the camera stays where it was; for an app drawing its own full screen. */
void scene_set_paused(bool paused);

/* Settings of the active camera. The scale is the share of the destination's
 * size it renders at, upscaled on the way out; the camera's def holds its clear colour. */
void scene_set_render_scale(int percent);

/* What the last draw kept after culling. */
raster_stats_t scene_stats(void);

#if !defined(ESP_PLATFORM) || CONFIG_LAUNCHER_DEVELOPMENT
/* Between draw and upscale, shows the frame as `mode` says; development only. */
void scene_set_debug_view(raster_show_t mode);
#endif
