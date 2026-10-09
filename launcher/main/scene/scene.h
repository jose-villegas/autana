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
 * A scene is loaded from its pack entry (SCNE), baked from its scene file,
 * and keeps a view of it: the pack must outlive the scene.
 * docs/render/Scene-Manager.md says how a frame goes.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "asset/asset_pack.h"
#include "render/r3d_instance.h"
#include "render/r3d_scene.h"

typedef struct scene scene_t;

/* An index into one scene's component arrays. */
typedef uint16_t scene_entity_t;
#define SCENE_ENTITY_NONE ((scene_entity_t)0xFFFFU)

/* Where an entity stands: the placement the raster reads. */
typedef r3d_placement_t scene_transform_t;

typedef enum {
    SCENE_OK = 0,
    SCENE_ERR_UNKNOWN, /* the pack holds no scene of that id */
    SCENE_ERR_FULL,    /* as many scenes are loaded as the manager holds */
    SCENE_ERR_MEMORY,  /* the scene's block could not be allocated */
    SCENE_ERR_ASSET,   /* no pack, or its entry, a mesh or its clip did not open: `asset` says why */
} scene_status_t;

/* What a load reports: its status, the pack's status when an entry failed,
 * and the id of the scene, mesh or clip it was about. A copy, since a failed
 * load releases the pack the id came from. */
typedef struct {
    scene_status_t status;
    asset_status_t asset;
    char what[ASSET_NAME_MAX];
} scene_failure_t;

/* Loads the scene `id` beside any already loaded: its entry, every mesh it
 * names and its camera's clip, from `pack`, or for scene_load() from the
 * asset pack `id`, held until the scene unloads. NULL on failure, and
 * `why`, which may be NULL, says why; a success sets it to SCENE_OK. */
scene_t* scene_load(const char* id, scene_failure_t* why);
scene_t* scene_load_from(const asset_pack_t* pack, const char* id, scene_failure_t* why);

/* Frees the scene. If its camera was the active one, nothing is drawn. */
void scene_unload(scene_t* scene);

/* The entity named `name`, or SCENE_ENTITY_NONE. The way to an entity: look
 * it up once, when the scene loads, and keep it. */
scene_entity_t scene_find(const scene_t* scene, const char* name);

/* Enumeration for tools selecting components by their authored names. */
int scene_entity_count(const scene_t* scene);
const char* scene_entity_name(const scene_t* scene, scene_entity_t entity);

/* The pack id of the mesh the entity draws, or NULL when it draws none. */
const char* scene_entity_mesh_id(const scene_t* scene, scene_entity_t entity);

/* The mesh the entity draws, or NULL when it draws none. */
const r3d_lit_mesh_t* scene_entity_mesh(const scene_t* scene, scene_entity_t entity);

/* The lens and path of the scene's camera `camera` (NULL: its first), to
 * sample without drawing; NULL when the scene has no such camera. */
const r3d_scene_camera_t* scene_camera_lens(const scene_t* scene, const char* camera);

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
