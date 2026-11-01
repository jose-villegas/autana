/*
 * scene: the engine's scene manager. Packed component arrays indexed by a
 * small entity id hold a transform and a parent per entity, and a
 * renderer, camera, animator or skinned renderer only for entities that
 * have one; animator and skin buffers are allocated when attached.
 */
#pragma once

#include "build/build_variant.h"
#if BUILD_VARIANT_DEVELOPMENT_OR_HOST
typedef struct scene_checkpoint scene_checkpoint_t;
/* Loaded scenes and their component allocations must outlive the checkpoint. */
scene_checkpoint_t* scene_save(void);
void scene_restore(const scene_checkpoint_t* checkpoint);
void scene_checkpoint_free(scene_checkpoint_t* checkpoint);
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "asset/asset_pack.h"
#include "hash/name_hash.h"
#include "render/r3d_instance.h"
#include "scene/camera/camera.h"

typedef struct scene scene_t;

/* An index into one scene's component arrays. */
typedef uint16_t scene_entity_t;
#define SCENE_ENTITY_NONE ((scene_entity_t)0xFFFFU)

typedef enum {
    SCENE_OK = 0,
    SCENE_ERR_UNKNOWN, /* the pack holds no scene of that id */
    SCENE_ERR_FULL,    /* as many scenes are loaded as the manager holds */
    SCENE_ERR_MEMORY,  /* the scene's block could not be allocated */
    SCENE_ERR_ASSET, /* no pack, or its entry, a mesh, its clip or an entity's placement did not open: `asset` says why */
} scene_status_t;

/* What a load reports: its status, the pack's status when an entry failed,
 * and the id of the scene, mesh or clip, or the entity name, it was about. A copy, since a failed
 * load releases the pack the id came from. */
typedef struct {
    scene_status_t status;
    asset_status_t asset;
    char what[ASSET_NAME_MAX];
} scene_failure_t;

/* Loads the scene `id` beside any already loaded: its entry, every mesh, skin,
 * skeleton and clip it names, from `pack`, or for scene_load() from the
 * asset pack `id`, held until the scene unloads. NULL on failure, and
 * `why`, which may be NULL, says why; a success sets it to SCENE_OK. */
scene_t* scene_load(const char* id, scene_failure_t* why);
scene_t* scene_load_from(const asset_pack_t* pack, const char* id, scene_failure_t* why);

/* Frees the scene. If its camera was the active one, nothing is drawn. */
void scene_unload(scene_t* scene);

/* Camera lookups below take this for the scene's first camera. */
#define SCENE_CAMERA_FIRST NAME_HASH_NONE

/* The entity whose name hashes to `name` (name_hash()), or SCENE_ENTITY_NONE.
 * The way to an entity: look it up once, when the scene loads, and keep it. */
scene_entity_t scene_find(const scene_t* scene, name_hash_t name);

/* Enumeration for tools selecting components by their authored names. */
int scene_entity_count(const scene_t* scene);
const char* scene_entity_name(const scene_t* scene, scene_entity_t entity);

/* The pack id of the mesh the entity draws, or NULL when it draws none. */
const char* scene_entity_mesh_id(const scene_t* scene, scene_entity_t entity);

/* The mesh the entity draws, or NULL when it draws none. */
const r3d_lit_mesh_t* scene_entity_mesh(const scene_t* scene, scene_entity_t entity);

/* The entity's renderer draws `mesh` instead of the one it loaded: a mesh
 * made at run time, which the caller keeps alive for the scene's life. The
 * scene keeps a copy of the view, so call again after each rebuild changes
 * its counts. A mesh with no nodes draws nothing. False when the entity
 * has no renderer or a skinned one, whose mesh its animator writes. */
bool scene_renderer_set_mesh(scene_t* scene, scene_entity_t entity, const r3d_lit_mesh_t* mesh);

/* The lens of the scene's camera named by `camera` (SCENE_CAMERA_FIRST: its first); NULL when the scene has no
 * such camera. */
const camera_t* scene_camera_lens(const scene_t* scene, name_hash_t camera);
/* Returns false for a non-camera entity or an invalid presentation. */
bool scene_camera_set_present(scene_t* scene, scene_entity_t camera, camera_present_t present);

/* Returns false for a non-camera entity or an invalid style. */
bool scene_camera_set_small_fill(scene_t* scene, scene_entity_t camera, small_fill_t fill);
/* Sets the camera's lens shift (render_view_t.shift): an off-axis view, as a
 * window onto a fixed rectangle needs. False for a non-camera entity or a
 * shift that is not finite. */
bool scene_camera_set_shift(scene_t* scene, scene_entity_t camera, vec2f_t shift);

/* Setters attach a directional light, initially white with zero intensity; refusals change nothing. */
bool scene_light_set_color(scene_t* scene, scene_entity_t entity, vec3f_t linear_rgb);
bool scene_light_set_intensity(scene_t* scene, scene_entity_t entity, float intensity);
/* Zero when the entity has no light. */
vec3f_t scene_light_color(const scene_t* scene, scene_entity_t entity);
float scene_light_intensity(const scene_t* scene, scene_entity_t entity);

/* The transform component: every entity has one. Its local transform
 * (position, rotation, scale) is stored, in its parent's frame; its world is
 * derived from its parents', and for a root the two are the same. */
transformf_t scene_entity_local(const scene_t* scene, scene_entity_t entity);
void scene_entity_set_local(scene_t* scene, scene_entity_t entity, transformf_t local);
bool scene_entity_enabled(const scene_t* scene, scene_entity_t entity);

/* A disabled entity's renderer is not drawn, nor its descendants'; every entity starts enabled. */
void scene_entity_set_enabled(scene_t* scene, scene_entity_t entity, bool enabled);

/* Drawn and animated: enabled, and every ancestor enabled. */
bool scene_entity_active(const scene_t* scene, scene_entity_t entity);

/* The entity's parent, or SCENE_ENTITY_NONE for a root. An entity's transform
 * is in its parent's frame. */
scene_entity_t scene_entity_parent(const scene_t* scene, scene_entity_t entity);

/* True when `entity` is `ancestor` or one of its descendants. */
bool scene_entity_is_within(const scene_t* scene, scene_entity_t entity, scene_entity_t ancestor);

/* Where the entity stands in the world: its local placed in its parents'.
 * Setting it stores the local that puts it there. */
transformf_t scene_entity_world(const scene_t* scene, scene_entity_t entity);
void scene_entity_set_world(scene_t* scene, scene_entity_t entity, transformf_t world);

/* The same world as a matrix, for applying it to many points: the form the
 * renderer draws with. */
mat4f_t scene_entity_world_matrix(const scene_t* scene, scene_entity_t entity);

/* Makes the scene's camera named by `camera` (SCENE_CAMERA_FIRST: its first)
 * the active one. False when the scene has no such camera. */
bool scene_activate(scene_t* scene, name_hash_t camera);

/* No camera is active, so nothing is drawn, until the next scene_activate(). */
void scene_deactivate(void);

/* While paused nothing advances or draws, as if no camera were active, and
 * the camera stays where it was; for an app drawing its own full screen. */
void scene_set_paused(bool paused);

typedef enum { SCENE_ANIMATOR_CURRENT, SCENE_ANIMATOR_PREVIOUS } scene_animator_slot_t;

/* skeleton_id NULL binds property clips to scene components. Asset ids are resolved once. */
bool scene_animator_set_clips(scene_t* scene, scene_entity_t entity, const char* skeleton_id, const char* const* clips,
                              int count, scene_failure_t* why);
bool scene_renderer_set_skin(scene_t* scene, scene_entity_t entity, const char* skin_id, scene_failure_t* why);
/* Property clips require blend_ms == 0. Skeleton clips support a transition blend. */
bool scene_animator_play(scene_t* scene, scene_entity_t entity, int clip, uint32_t blend_ms);
bool scene_animator_set_speed(scene_t* scene, scene_entity_t entity, scene_animator_slot_t slot, float speed);
bool scene_animator_set_paused(scene_t* scene, scene_entity_t entity, scene_animator_slot_t slot, bool paused);
int scene_animator_clip_count(const scene_t* scene, scene_entity_t entity);
const char* scene_animator_clip_name(const scene_t* scene, scene_entity_t entity, int clip);
/* The clip the current slot plays, or -1 when none does. */
int scene_animator_current_clip(const scene_t* scene, scene_entity_t entity);

uint32_t scene_animator_clip_duration_ms(const scene_t* scene, scene_entity_t entity, int clip);
/* Seek samples the scene systems immediately, without drawing. */
bool scene_animator_seek(scene_t* scene, scene_entity_t entity, scene_animator_slot_t slot, uint32_t t_ms);
