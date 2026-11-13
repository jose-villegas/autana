/*
 * scene_internal: the scene's columns and the world's records, shared by
 * scene.c (load), entity/entity.c (the entity table), the component modules,
 * scene_animation.c, scene_hierarchy.c and scene_draw.c, and the SCNE entry
 * scene_asset.c reads. Not for apps.
 */
#pragma once

#include "anim/anim_tracks.h"
#include "anim/playback/anim_layer.h"
#include "core/checkpoint.h"
#include "core/handle.h"
#include "core/memory.h"
#include "core/pending.h"
#include "hash/name_hash.h"
#include "math/linear/mat3f.h"
#include "render/batch/r3d_batch.h"
#include "render/lod/r3d_lod.h"
#include "render/r3d_instance.h"
#include "render/skin/r3d_skin_light.h"
#include "scene/animator/animator.h"
#include "scene/camera/camera.h"
#include "scene/entity/entity.h"
#include "scene/scene.h"
#include "scene/skin/skin.h"

typedef struct scene scene_t;
#include "scene/scene_conventions.h"
#include "scene/scene_format.h"
#include "scene/scene_shell.h"

/* Absent ambient contributes zero. */
#define SCENE_AMBIENT_INTENSITY_NONE  0.0F
#define SCENE_TONEMAP_WHITE_NONE      0.0F
#define SCENE_LIGHT_INTENSITY_DEFAULT 0.0F
#define SCENE_LIGHT_COLOR_DEFAULT     vec3f_one()

typedef enum { SCENE_LIGHT_NONE, SCENE_LIGHT_DIRECTIONAL } scene_light_kind_t;

typedef struct {
    scene_light_kind_t kind;
    vec3f_t color;
    float intensity;
} scene_light_t;

#define SCENE_FLAG_ENABLED         1U
#define SCENE_FLAG_DIRTY           2U
#define SCENE_FLAG_SKIN_DIRTY      4U
#define SCENE_FLAG_CHANGED         (SCENE_FLAG_DIRTY | SCENE_FLAG_SKIN_DIRTY)
#define SCENE_FLAG_ACTIVE          8U  /* enabled, and every ancestor enabled */
#define SCENE_FLAG_MOVED           16U /* its world changed in this resolve */
/* Its transform was set since its skin was last lit: a copy whose playing
 * clip paces it relights at its next pose, any other at once; a moved light
 * relights every skin at once. */
#define SCENE_FLAG_RELIGHT         32U
#define SCENE_FLAG_PLACED          (SCENE_FLAG_DIRTY | SCENE_FLAG_RELIGHT)
#define SCENE_WORLD_SLOT_NONE      0xFFFFU
#define SCENE_LIGHT_FAILURE_ALBEDO "skin vertex colors required"
#define SCENE_LIGHT_FAILURE_NORMAL "skin normal transform invalid"

/* A row of one scene's columns: the SCNE entity index. Apps hold entity_t
 * handles; the world (entity.c) turns one into its scene and row. */
typedef uint16_t scene_row_t;
#define SCENE_ROW_NONE ((scene_row_t)0xFFFFU)
#define SCENE_ROW_ROOT ((scene_row_t)0xFFFEU) /* the scene's root entity, outside its columns */

typedef struct {
    mat3f_t linear;
    vec3f_t position;
} scene_asset_transform_t;

/* The rows of the SCNE entry (scene_format.h) are read in place, so they are
 * the entry's bytes, little-endian as the device is. */
typedef struct {
    name_hash_t hash; /* name_hash(name), never NAME_HASH_NONE */
    char name[ASSET_NAME_MAX];
} scene_asset_name_t;

typedef struct {
    uint16_t entity, colour_set;
    char mesh[ASSET_NAME_MAX];
} scene_asset_renderer_t;

/* A renderer that draws copies of its mesh from the packed rows below
 * (docs/assets/formats/SCNE.md#batches). */
typedef struct {
    uint16_t renderer, copy_count;
    uint32_t first_copy, first_chunk; /* into places and turns, and into chunks */
} scene_asset_batch_t;

typedef struct {
    uint16_t entity, pad;
    float half_fov_short_tan, near_z;
    uint32_t clear_rgb; /* 0xRRGGBB */
} scene_asset_camera_t;

typedef struct {
    vec3f_t ambient_color;
    float ambient_intensity, tonemap_white;
} scene_asset_settings_t;

typedef struct {
    uint16_t entity;
    uint8_t kind, pad;
    vec3f_t color;
    float intensity;
} scene_asset_light_t;

typedef struct {
    uint16_t entity, pad;
    char mesh[ASSET_NAME_MAX], skin[ASSET_NAME_MAX], skeleton[ASSET_NAME_MAX];
} scene_asset_skinned_renderer_t;

typedef struct {
    uint16_t entity, clip_count;
    uint32_t first_clip;
    uint16_t default_clip;
    uint8_t wrap, pad;
} scene_asset_animator_t;

/* An opened entry. Holds no copy: the pack must outlive it. */
typedef struct {
    uint16_t entity_count, renderer_count, camera_count;
    uint16_t light_count, skinned_renderer_count, animator_count, clip_count;
    uint16_t batch_count, chunk_count, copy_count;
    const scene_asset_name_t* names;
    const scene_asset_transform_t* transforms;
    const scene_asset_renderer_t* renderers;
    const scene_asset_camera_t* cameras;
    const scene_asset_light_t* lights;
    const scene_asset_skinned_renderer_t* skinned_renderers;
    const scene_asset_animator_t* animators;
    const char (*clips)[ASSET_NAME_MAX];
    const scene_asset_settings_t* settings;
    const uint16_t* parents; /* one per entity: SCENE_ASSET_PARENT_ROOT or a lower entity */
    const scene_asset_batch_t* batches;
    const r3d_copy_chunk_t* chunks;
    const r3d_copy_place_t* places; /* copy_count */
    const r3d_copy_turn_t* turns;   /* copy_count */
} scene_asset_t;

/* Checks every row once and returns the first failure: ASSET_ERR_VERSION,
 * ASSET_ERR_BOUNDS or ASSET_ERR_FORMAT. Else fills `out`. */
asset_status_t scene_asset_open(asset_view_t entry, scene_asset_t* out);

/* A mesh view and its world matrix: its parents' transforms times its entity's own. */
typedef struct {
    r3d_lit_mesh_t mesh;
    mat4f_t model;
    box3f_t world_bounds; /* the mesh's root box through `model`, kept with it */
    box3f_t drawn_bounds; /* world_bounds when last drawn (render/partial) */
    scene_row_t entity;
    bool placed;        /* false while the transform is the identity: drawn as it is */
    bool changed;       /* its mesh or model changed since it was last drawn (render/partial) */
    uint32_t lod_words; /* the level state held for the mesh opened: R3D_LOD_STATE_WORDS of its groups, a copy's each */
    uint32_t* lod_coarse; /* into the scene's lod_bits, or NULL without levels; a batch's are its copies' */
    r3d_batch_t* batch;   /* its copies, or NULL: it draws its mesh once */
    uint16_t colour_sets; /* the sets its colour arrays hold from the one opened: 1, or its batch's highest plus one */
} scene_renderer_t;

/* Sets `r`'s world box from its mesh and model; call after either changes,
 * a skinned mesh's refit included. */
void scene_renderer_bound(scene_renderer_t* r);

typedef struct {
    lens_t lens;
    scene_row_t entity;
    uint16_t clear; /* panel pixel format */
} scene_camera_t;

typedef struct {
    anim_tracks_t tracks;
    const char* id;
    anim_joint_bound_t* joints;
    anim_bound_t* fields;
} scene_animator_clip_t;

enum { SCENE_ANIM_TIER_NEAR, SCENE_ANIM_TIER_MID, SCENE_ANIM_TIER_FAR };

typedef struct {
    anim_layer_t layer;
    anim_skeleton_t skeleton;
    scene_animator_clip_t* clips;
    void* storage;
    const anim_joint_bound_t** joint_tables;
    int* counts;
    uint8_t root;
    anim_wrap_t wrap;
    float drawn_radius_px; /* last draw's size; cleared each step so undrawn scenes use the far tier */
    uint32_t owed_ms;      /* skipped time is charged at the next pose, before playback controls change */
    uint32_t phase;        /* stable slot spreads copies' poses across frames */
    int interval;          /* zero selects the first tier without a hysteresis history */
    int tier;              /* retained inside the boundary band to avoid rate flicker */
    bool dirty, posed;
} scene_animator_t;

typedef struct {
    r3d_skin_t skin;
    r3d_lit_mesh_t bind;
    r3d_skin_output_t output;
    mat4f_t* model;
    mat4f_t* palette;
    void* storage;
    uint8_t (*colors)[R3D_SKIN_LIGHT_CHANNELS];
    mat3f_t* normal_palette;
    void* light_storage;
    bool lighting_valid;
    /* bound: a skin is attached. Its buffers (storage, light_storage)
     * exist only while its entity is ACTIVE; starved: they did not fit when it
     * became ACTIVE, so it is not drawn until it stops being ACTIVE. */
    bool bound, starved;
    skin_light_mode_t light_mode;
} scene_skinned_renderer_t;

/* A scene's root entity and its load request. It exists from the request
 * until its release, before the scene's columns do and after a failure. */
typedef struct {
    entity_t root; /* ENTITY_NONE: a free record */
    handle_t load; /* its pack request, or HANDLE_NONE for scene_load_from() */
    pending_t state;
    bool async;
    pending_fn done; /* NULL once delivered or cancelled */
    handle_t owner;
    uint64_t order; /* request order: requests made inside a callback wait a pass */
    scene_failure_t failure;
    char id[ASSET_NAME_MAX];
    transformf_t local;
    mat4f_t world;
    uint8_t flags;       /* SCENE_FLAG_ENABLED, ACTIVE, DIRTY, MOVED */
    bool placed;         /* `world` is not the identity */
    uint16_t base, rows; /* the slots of its rows: [base, base + rows) */
    struct scene* scene; /* its columns, while they exist */
} scene_root_t;

/* Loaded and pending scenes at once. */
#define SCENE_ROOTS_MAX 8
extern scene_root_t scene_roots[SCENE_ROOTS_MAX];

/* The record and row `entity` names, or NULL when it is ENTITY_NONE, stale or
 * another owner's; a development device build logs and asserts on the last two. */
scene_root_t* scene_entity_lookup(entity_t entity, scene_row_t* row);
/* The same, never logging: for questions such as whether an owner is alive. */
scene_root_t* scene_entity_peek(entity_t entity, scene_row_t* row);
/* The scene and row of an entity in a loaded scene's columns, or NULL. */
struct scene* scene_entity_row(entity_t entity, scene_row_t* row);
entity_t scene_row_entity(const struct scene* scene, scene_row_t row);
/* Issues a fresh root entity into `record`; false when no slot is left. */
bool scene_world_issue_root(scene_root_t* record);
/* Reserves `rows` slots for the record's columns, in one run; false when none is long enough. */
bool scene_world_issue_rows(scene_root_t* record, int rows);

struct scene {
    scene_asset_t asset;
    scene_root_t* root;
    int renderer_count;
    uint32_t animation_frame; /* scene-local clock makes the stagger independent of other scenes */
    uint32_t animation_phase; /* next slot, assigned once so rebinding keeps the stagger */
    int animator_count, skeleton_animator_count;
    int16_t animate_share, skin_share; /* core 1's, as core/job.h steers them */
    int light_count;
    r3d_skin_lights_t world_lights;
    scene_light_t* lights;
    vec3f_t ambient_color;
    float ambient_intensity, tonemap_white;
    r3d_skin_light_transfer_t* light_transfer;
    r3d_skin_light_table_t* light_table;
    int light_table_users;
    bool light_table_dirty;
    bool skins_pending; /* a bound skin may be ACTIVE without its buffers */
    int starved_count;  /* skins whose buffers did not fit; root->failure says which */
    uint8_t* flags;     /* one per entity: SCENE_FLAG_* */
    scene_renderer_t* renderers;
    void* resident; /* mesh arrays (positions, triangles, colours), deduplicated; NULL when absent */
    scene_camera_t* cameras;
    const asset_pack_t* pack;
    transformf_t* locals;
    scene_animator_t* animators;
    scene_skinned_renderer_t* skinned_renderers;
    r3d_instance_t* instances;     /* one per renderer: what the raster draws this frame */
    r3d_batch_t* batches;          /* one per SCNE batch, its renderer's */
    r3d_batch_draw_t* batch_draws; /* one per SCNE batch: what the raster draws of it this frame */
    r3d_copy_chunk_t* chunks;      /* every batch's chunks, internal RAM: what the cull reads first */
    uint16_t* world_slots;         /* one per entity: its index in `worlds`, or SCENE_WORLD_SLOT_NONE when childless */
    int world_count;
    mat4f_t* worlds;             /* one per entity with children, in entity order */
    scene_row_t* world_entities; /* the entity each of `worlds` belongs to */
    uint32_t* lod_bits;          /* every renderer's coarse bits, kept from frame to frame */
    /* The per-frame loops walk these, not every entity or renderer: a scene
     * with many disabled copies has few of either. */
    scene_row_t* staged; /* entities with an animator, a skin or a light, in entity order */
    int staged_count;
    uint16_t* active_renderers; /* renderers whose entity is ACTIVE, in renderer order */
    int active_renderer_count;
    bool active_renderers_stale; /* an ACTIVE bit flipped since the list was built */
};

#if !defined(ESP_PLATFORM) || CONFIG_LAUNCHER_DEVELOPMENT
/* Fault injection exercises allocation rollback without exhausting the heap. */
extern int scene_fail_allocation_after;
#endif

void* scene_allocate(size_t bytes, memory_kind_t kind);

/* A PSRAM block that lives as long as its scene: from the take that
 * scene_set_take() installed, else the heap. scene_free_kept() frees it only
 * when it came from the heap; a taken one goes back with its lender. */
void* scene_allocate_kept(size_t bytes);
void scene_free_kept(void* block);

/* Copies static mesh arrays (positions, triangles, colours) into one block from scene_allocate_kept(),
 * deduplicated per array, and points the renderers at it. SCENE_ERR_MEMORY when the block cannot be had. */
scene_failure_t scene_mesh_make_resident(scene_t* scene);

/* The columns of world record `index` (below SCENE_ROOTS_MAX) when it is READY, else NULL. */
scene_t* scene_ready_at(int index);

/* Only activation and a seek of the active camera count as cuts. Other discontinuities,
 * such as a camera transform teleport or a clip switch, converge within a few frames. */
void scene_draw_camera_cut(const scene_t* scene, scene_row_t entity);

/* The index of the camera on `entity`, or -1. */
int scene_camera_index(const scene_t* scene, scene_row_t entity);

/* Draws through the scene's camera `camera` (an index) from now on. */
void scene_draw_activate(scene_t* scene, int camera);

/* A scene is going away: the draw drops it if its camera is active. */
void scene_draw_forget(const scene_t* scene);
void scene_draw_release(void);

/* Rebuilds the world of every parent and active renderer whose own transform
 * or an ancestor's changed, then clears DIRTY and MOVED. An inactive
 * renderer's world waits: turning ACTIVE marks it CHANGED. */
void scene_resolve(scene_t* scene);

/* Row-level component access for the public modules; `row` may be
 * SCENE_ROW_ROOT, and the record's scene is NULL only for a root. */
scene_row_t scene_row_parent(const scene_t* scene, scene_row_t row); /* SCENE_ROW_NONE: under the root */
transformf_t scene_row_local(const scene_root_t* root, scene_row_t row);
void scene_row_set_local(scene_root_t* root, scene_row_t row, transformf_t local);
transformf_t scene_row_world(const scene_root_t* root, scene_row_t row);
void scene_row_set_world(scene_root_t* root, scene_row_t row, transformf_t world);
mat4f_t scene_row_world_matrix(const scene_root_t* root, scene_row_t row);
uint8_t scene_row_flags(const scene_root_t* root, scene_row_t row);
void scene_row_set_enabled(scene_root_t* root, scene_row_t row, bool enabled);

/* Numbers the entities with children and allocates their worlds; false when it could not. */
bool scene_hierarchy_open(scene_t* scene);
void scene_hierarchy_release(scene_t* scene);

/* The entity's enabled flag changed: recomputes ACTIVE for it and every
 * later entity, descendants or not; parents come before children, so one
 * forward pass is correct. O(entities) a call, cheap at today's counts; if
 * they grow, a later entity whose parent is before `entity` can be skipped,
 * since it cannot be a descendant. */
void scene_hierarchy_update_active(scene_t* scene, scene_row_t entity);

/* True when drawing the mesh as it is gives the same picture. */
bool scene_transform_is_identity(const mat4f_t* transform);

size_t scene_storage_carve(size_t* end, size_t bytes, size_t align);
#define SCENE_STORAGE_RESERVE(layout, end, member, type, count)                                                        \
    ((layout).member = scene_storage_carve(&(end), sizeof(type) * (size_t)(count), _Alignof(type)))
int scene_renderer_index(const scene_t* scene, scene_row_t entity);
#if BUILD_VARIANT_DEVELOPMENT_OR_HOST
void scene_draw_checkpoint_capture(checkpoint_t* saved);
void scene_world_checkpoint_capture(checkpoint_t* saved);
void scene_checkpoint_capture(checkpoint_t* saved, scene_t* scene);
void scene_animation_checkpoint_capture(checkpoint_t* saved, scene_t* scene);
#endif
void scene_animation_release(scene_t* scene);
/* An animator or light now exists on `entity`: adds it to `staged` once. A
 * skin needs its entity's skeleton animator, so its entity is staged already. */
void scene_stage_add(scene_t* scene, scene_row_t entity);
/* Settles the skins' buffers, then advances and skins. */
void scene_animation_step(scene_t* scene, uint32_t dt_ms);
/* The same without settling: a seek re-poses what is shown and takes no buffers. */
void scene_animation_repose(scene_t* scene, uint32_t dt_ms);
/* Animates staged[first, first + count). */
void scene_animate_range(scene_t* scene, int first, int count, uint32_t dt_ms, const render_anim_settings_t* settings);
bool scene_skin_light_apply_mode(scene_t* scene, scene_row_t entity, scene_skinned_renderer_t* skin,
                                 skin_light_mode_t mode);
void scene_skin_light_table_release(scene_t* scene, const scene_skinned_renderer_t* skin);
/* Attaches the skin `id` to the row's renderer. With `take` an ACTIVE row
 * takes its buffers now, and on SCENE_ERR_MEMORY keeps its old skin; without,
 * they wait for scene_skins_settle(). */
bool scene_renderer_bind_skin(scene_t* scene, scene_row_t row, const char* id, bool take, scene_failure_t* why);
/* Takes the skin's buffers and skins them in its animator's shown pose; false,
 * with nothing taken, when they do not fit. */
bool scene_skin_take(scene_t* scene, scene_row_t entity, scene_skinned_renderer_t* skin);
/* Frees the skin's buffers, if it holds them. */
void scene_skin_give(scene_t* scene, scene_skinned_renderer_t* skin);
/* Points the entity's renderer at its skin's output, or at the bind mesh when
 * it holds no buffers, and marks it for a re-skin and relight. */
void scene_skin_show(scene_t* scene, scene_row_t entity);
/* Takes the buffers of every ACTIVE skin without them; one that does not fit
 * is starved and reported through scene_failure(). */
void scene_skins_settle(scene_t* scene);
/* The entity stopped being ACTIVE: its skin gives its buffers back. */
void scene_skin_deactivate(scene_t* scene, scene_row_t entity);
bool scene_skin_light_allocate(scene_skinned_renderer_t* skin);
void scene_skin_light_prepare(scene_t* scene, scene_row_t entity);
bool scene_light_color_fits(vec3f_t linear_rgb);
r3d_skin_light_transfer_t* scene_light_transfer_take(const scene_t* scene);
bool scene_lights_allocate(scene_t* scene);
void scene_lights_prepare(scene_t* scene);
void scene_skin_range(scene_t* scene, int first, int count);

scene_failure_t scene_failure_make(scene_status_t status, asset_status_t asset, const char* what);
/* Stores the failure in `why`, which may be NULL; true for SCENE_OK. */
bool scene_report(scene_failure_t* why, scene_status_t status, asset_status_t asset, const char* what);
