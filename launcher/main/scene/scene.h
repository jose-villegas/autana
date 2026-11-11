/*
 * scene: loads scenes into the engine's one world. Each loaded scene is a
 * subtree under its root entity; its components are reached through their
 * modules (entity, transform, renderer, skin, animator, camera, light).
 * Loading follows the asset store's: the plain call blocks, _async returns
 * the root at once and completes from the frame loop.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "asset/asset_pack.h"
#include "core/pending.h"
#include "math/geometry/box3f.h"
#include "scene/entity/entity.h"

typedef enum {
    SCENE_OK = 0,
    SCENE_ERR_UNKNOWN, /* the pack holds no scene of that id */
    SCENE_ERR_FULL,    /* the world has no run of entity slots long enough */
    SCENE_ERR_MEMORY,  /* the scene's block could not be allocated */
    SCENE_ERR_ASSET, /* no pack, or its entry, a mesh, its clip or an entity's placement did not open: `asset` says why */
} scene_status_t;

/* Why a load failed: its status, the pack's status when an entry failed,
 * and the id of the scene, mesh or clip, or the entity name, it was about. */
typedef struct {
    scene_status_t status;
    asset_status_t asset;
    char what[ASSET_NAME_MAX];
} scene_failure_t;

/* Loads the scene `id` from the asset pack it names, or for scene_load_from()
 * from `pack`, which outlives it, beside any already loaded, and returns its
 * root: READY or FAILED, or ENTITY_NONE when as many scenes are held as the
 * world holds. A failure logs one reason. Release every root.
 * scene_load()'s and scene_load_async()'s id is an asset id (asset_id.h),
 * "pack/scene", or a bare "scene" in the pack of its own name; an id it
 * refuses fails with SCENE_ERR_ASSET and ASSET_ERR_FORMAT. */
entity_t scene_load(const char* id);
entity_t scene_load_from(const asset_pack_t* pack, const char* id);

/* Returns the root at once; the scene opens on the frame loop once its pack
 * is READY. Completion runs from the frame loop on core 0, never inside this
 * call. Owners as asset_store_load_async(): HANDLE_NONE is the running app's,
 * a token groups loads, and an entity owner ends with its scene. Until READY
 * the root can be placed and enabled, and holds nothing to find. */
entity_t scene_load_async(const char* id, pending_fn done, handle_t owner);
pending_t scene_state(entity_t root);
/* False unless the load FAILED, or a skinned entity's buffers did not fit
 * when it became active (it is not drawn until it stops being active); then
 * `why` says why. */
bool scene_failure(entity_t root, scene_failure_t* why);
/* Suppresses completion; before READY also frees partial work. READY keeps its scene. */
void scene_cancel(entity_t root);
/* Unloads the scene in any state: its entities are refused from now on. If
 * its camera was the active one, nothing is drawn. */
void scene_release(entity_t root);

/* The world box around everything the scene holding `entity` (its root or
 * any entity below) draws now, its active renderers with a mesh, a sky or
 * backdrop mesh among them; empty when it draws nothing or is not READY. */
box3f_t scene_bounds(entity_t entity);

/* While paused nothing advances or draws, as if no camera were active, and
 * the camera stays where it was; for an app drawing its own full screen. */
void scene_set_paused(bool paused);
