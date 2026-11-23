/*
 * scene_internal: the scene's columns and the world's records, shared by
 * scene.c (load), entity/entity.c (the entity table), the component modules,
 * scene_animation.c, scene_hierarchy.c and scene_draw.c, and the SCNE entry
 * scene_asset.c reads. Not for apps.
 */
#pragma once

#include <limits.h>

#include "anim/anim_tracks.h"
#include "anim/playback/anim_layer.h"
#include "asset/asset_store.h"
#include "build/build_variant.h"
#include "collections/mem.h"
#include "core/checkpoint.h"
#include "core/handle.h"
#include "core/memory.h"
#include "core/pending.h"
#include "hash/name_hash.h"
#include "math/linear/mat3f.h"
#include "render/batch/r3d_batch.h"
#include "render/lod/r3d_lod.h"
#include "render/probe/sh_probes.h"
#include "render/r3d_instance.h"
#include "render/skin/r3d_skin_light.h"
#include "scene/animator/animator.h"
#include "scene/camera/camera.h"
#include "scene/constraint/constraint.h"
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

#include "scene/scene_asset.h"

/* A mesh view and its world matrix: its parents' transforms times its entity's own. */
typedef struct {
    r3d_lit_mesh_t mesh;
    mat4f_t model;
    box3f_t world_bounds; /* the mesh's root box through `model`, kept with it */
    float contact_blob_ground_y, contact_blob_strength;
    box3f_t drawn_bounds; /* world_bounds when last drawn (render/partial) */
    scene_row_t entity;
    bool placed;        /* false while the transform is the identity: drawn as it is */
    bool changed;       /* its mesh or model changed since it was last drawn (render/partial) */
    uint32_t lod_words; /* the level state held for the mesh opened: R3D_LOD_STATE_WORDS of its groups, a copy's each */
    uint32_t* lod_coarse; /* into the scene's lod_bits, or NULL without levels; a batch's are its copies' */
    r3d_batch_t* batch;   /* its copies, or NULL: it draws its mesh once */
    uint16_t colour_sets; /* the sets its colour arrays hold from the one opened: 1, or its batch's highest plus one */
    void* texturing;      /* its pack mesh's textures once opened (render/texture/), at load when they cut, or NULL */
    bool texturing_owner; /* the renderer that opened them: the scene holds one block per mesh id */
    /* Its static arrays are copied into PSRAM when it is first ACTIVE
     * (scene_mesh_take()); a renderer never shown takes none. */
    uint8_t resident_arrays; /* a bit per array it draws from a copy; SCENE_RESIDENT_ALL: nothing left to copy */
    bool starved;            /* its copies did not fit when it became ACTIVE: not drawn until it stops being ACTIVE */
    void* resident;          /* its take of copies, or NULL */
#if BUILD_VARIANT_DEVELOPMENT_OR_HOST
    asset_copies_t* copies; /* resident's arrays' sources, told to the asset store; NULL when none */
#endif
} scene_renderer_t;

#define SCENE_RESIDENT_ALL 0xFFU

/* Drops `r`'s textures, freeing them when it opened them; at the scene's close. */
void scene_renderer_texturing_free(scene_renderer_t* r);

/* Opens, at load, the textures of every static renderer whose mesh cuts
 * (r3d_texture_mesh_cuts()), so a MASK1 chart never draws plain. Fails with
 * SCENE_ERR_ASSET when they cannot open, or ASSET_ERR_FORMAT for a flat-shaded
 * mesh, which has no vertex colours to light them. */
scene_failure_t scene_renderer_open_cuts(scene_t* scene);

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

enum {
    ANIMATOR_FIELD_CLIP,
    ANIMATOR_FIELD_SPEED,
    ANIMATOR_FIELD_BLEND_MS,
    ANIMATOR_FIELD_COUNT,
};

extern const anim_component_fields_t ANIMATOR_FIELDS;

#define SCENE_COMPONENT_LIST(X)                                                                                        \
    X(TRANSFORM, ANIM_POSE_TRANSFORM_FIELDS, ANIM_POSE_TRANSFORM_FIELD_COUNT, NONE)                                    \
    X(LENS, LENS_FIELDS, LENS_FIELD_COUNT, TRANSFORM)                                                                  \
    X(ANIMATOR, ANIMATOR_FIELDS, ANIMATOR_FIELD_COUNT, LENS)                                                           \
    X(CNST, CONSTRAINT_FIELDS, CONSTRAINT_FIELD_COUNT, ANIMATOR)

#define SCENE_COMPONENT_ID(name, table, count, prior) SCENE_COMPONENT_##name,

enum { SCENE_COMPONENT_LIST(SCENE_COMPONENT_ID) SCENE_COMPONENT_COUNT };

#define SCENE_COMPONENT_RANGE(name, table, count, prior)                                                               \
    SCENE_FIELD_BASE_##name = SCENE_FIELD_END_##prior, SCENE_FIELD_END_##name = SCENE_FIELD_BASE_##name + count,

enum { SCENE_FIELD_END_NONE = 0, SCENE_COMPONENT_LIST(SCENE_COMPONENT_RANGE) };

typedef uint32_t scene_field_mask_t;

enum {
    SCENE_PROPERTY_TRANSFORM = 1U << SCENE_FIELD_END_CNST,
    SCENE_PROTECTED_TRANSFORM = SCENE_PROPERTY_TRANSFORM << 1,
};

_Static_assert(SCENE_FIELD_END_CNST + 2 <= sizeof(scene_field_mask_t) * CHAR_BIT,
               "scene animation fields fit in the claim mask");

static inline scene_field_mask_t
scene_field_bit(int component, int field) {
#define SCENE_COMPONENT_BIT(name, table, count, prior)                                                                 \
    case SCENE_COMPONENT_##name: return (scene_field_mask_t)(1U << (SCENE_FIELD_BASE_##name + field));
    switch (component) {
        SCENE_COMPONENT_LIST(SCENE_COMPONENT_BIT)
        default: return 0;
    }
}

typedef struct {
    anim_blocks_t blocks;
    scene_row_t entity;
} scene_timeline_driver_t;

typedef struct {
    int drivers;
    size_t clip_keys, time_keys;
    uint32_t* times_ms;
    uint8_t* slots;
    size_t target_keys;
    scene_row_t* targets;
} scene_timeline_blocks_t;

/* Separate from property animators so the scene root has one shared clock. */
typedef struct {
    anim_tracks_t tracks;
    anim_playhead_t clock;
    uint32_t applied_ms;
    int field_count, driver_count;
    scene_timeline_driver_t* drivers;
    const uint32_t* times_ms;
    const uint8_t* slots;
    const scene_row_t* target_keys;
    bool dirty;
    anim_bound_t fields[];
} scene_timeline_t;

typedef struct {
    const uint32_t* times;
    const scene_row_t* targets;
    const float* values;
    uint16_t count;
} scene_constraint_curve_t;

typedef struct {
    scene_asset_constraint_t values;
    transformf_t own;
    scene_row_t targets[CONSTRAINT_TARGET_COUNT];
    scene_constraint_curve_t curves[CONSTRAINT_STEP_COUNT];
} scene_constraint_t;

/* Temporary component references let the binder address scene columns directly. */
typedef struct {
    anim_target_t* targets;
    void* block; /* Owns targets and their component references. */
    int count;
} scene_anim_targets_t;

typedef struct {
    const scene_asset_t* asset;
    scene_field_mask_t* writes;
} scene_claims_t;

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
    float drawn_radius_px;    /* last draw's size when due; cleared each step so undrawn scenes use the far tier */
    uint32_t owed_ms;         /* skipped time is charged at the next pose, before playback controls change */
    uint32_t phase;           /* stable slot spreads copies' poses across frames */
    int interval;             /* zero selects the first tier without a hysteresis history */
    int tier;                 /* retained inside the boundary band to avoid rate flicker */
    uint16_t timeline_driver; /* One-based index in the scene timeline. */
    bool dirty, posed, timeline_pose_pending;
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
    const sh_probes_t* probes;
    bool skins_pending; /* a bound skin may be ACTIVE without its buffers */
    int starved_count;  /* skins and renderers whose memory did not fit; root->failure says which */
    scene_field_mask_t* writes;
    uint8_t* flags; /* one per entity: SCENE_FLAG_* */
    scene_renderer_t* renderers;
    scene_camera_t* cameras;
    const asset_pack_t* pack;
    transformf_t* locals;
    scene_animator_t* animators;
    scene_timeline_t* timeline;
    scene_constraint_t* constraints;
    size_t block_bytes;
    scene_skinned_renderer_t* skinned_renderers;
    r3d_batch_t* batches;     /* one per SCNE batch, its renderer's */
    r3d_copy_chunk_t* chunks; /* every batch's chunks, internal RAM: what the cull reads first */
    uint16_t* world_slots;    /* one per entity: its index in `worlds`, or SCENE_WORLD_SLOT_NONE when childless */
    int world_count;
    mat4f_t* worlds;             /* one per entity with children, in entity order */
    scene_row_t* world_entities; /* the entity each of `worlds` belongs to */
    uint32_t* lod_bits;          /* every renderer's coarse bits, kept from frame to frame */
    size_t lod_bit_words;        /* how many words lod_bits holds, as allocated */
    /* The per-frame loops walk these, not every entity or renderer: a scene
     * with many disabled copies has few of either. */
    scene_row_t* staged; /* entities with an animator, a skin or a light, in entity order */
    int staged_count;
    uint16_t* active_renderers; /* renderers whose entity is ACTIVE, in renderer order */
    int active_renderer_count;
    bool active_renderers_stale; /* an ACTIVE bit flipped since the list was built */
};

static inline bool
scene_is_lit(const scene_t* scene) {
    return scene->probes != NULL || scene->light_count > 0 || scene->ambient_intensity > 0.0F;
}

#if !defined(ESP_PLATFORM) || CONFIG_LAUNCHER_DEVELOPMENT
/* Fault injection exercises allocation rollback without exhausting the heap. */
extern int scene_fail_allocation_after;
#endif

void* scene_allocate(size_t bytes, memory_kind_t kind);

/* A PSRAM block that lives as long as its scene: from the take that
 * scene_set_take() installed, else the heap. scene_free_kept() frees it only
 * when it came from the heap; a taken one goes back with its lender. */
void* scene_allocate_kept(size_t bytes);
/* As scene_allocate_kept(), for what a load does without when refused: no
 * injected failure lands on it. */
void* scene_allocate_kept_optional(size_t bytes);
void scene_free_kept(void* block);

/* Copies static renderer `index`'s arrays (positions, triangles, colours) and
 * its batch's copy places not yet resident into one block from
 * scene_allocate_kept(), and points it, and every renderer sharing an array,
 * at the copies. False, with nothing kept, when the block cannot be had. */
bool scene_mesh_take(scene_t* scene, int index);
/* Frees `r`'s take when it came from the heap; at the scene's close. */
void scene_mesh_release(scene_renderer_t* r);

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
/* What the last draw gave the raster: every drawn scene's instances, in
 * the order the scenes are drawn; valid until the next draw. */
const r3d_instance_t* scene_drawn_instances(void);

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

#define SCENE_STORAGE_RESERVE(layout, end, member, type, count)                                                        \
    ((layout).member = mem_carve(&(end), sizeof(type) * (size_t)(count), _Alignof(type)))
int scene_renderer_index(const scene_t* scene, scene_row_t entity);
#if BUILD_VARIANT_DEVELOPMENT_OR_HOST
void scene_draw_checkpoint_capture(checkpoint_t* saved);
void scene_world_checkpoint_capture(checkpoint_t* saved);
void scene_checkpoint_capture(checkpoint_t* saved, scene_t* scene);
void scene_animation_checkpoint_capture(checkpoint_t* saved, scene_t* scene);
#endif
void scene_animation_release(scene_t* scene);
scene_anim_targets_t scene_anim_targets(const scene_asset_t* asset, const scene_t* scene);
bool scene_field_claim(scene_claims_t* claims, scene_row_t entity, scene_field_mask_t field);
scene_failure_t scene_animation_claim_clip(scene_claims_t* claims, const scene_anim_targets_t* targets,
                                           const anim_tracks_t* tracks, const char* clip, int self,
                                           scene_field_mask_t* owned);
void scene_animation_apply_fields(scene_t* scene, const anim_bound_t* fields, int count,
                                  const anim_playhead_t* playhead, const anim_clip_t* clip);
asset_status_t scene_animator_read_clip(const asset_pack_t* pack, const char* id, uint8_t root, anim_tracks_t* out);
bool scene_animator_set_clips(scene_t* scene, scene_row_t row, const char* skeleton_id, const char* const* ids,
                              int count, bool replace, scene_failure_t* why);
scene_failure_t scene_timeline_resolve(const scene_asset_t* asset, const scene_anim_targets_t* targets,
                                       scene_field_mask_t* writes, const anim_tracks_t* timeline,
                                       scene_timeline_blocks_t* blocks);
scene_failure_t scene_timeline_bind(scene_t* scene);
void scene_timeline_step(scene_t* scene, uint32_t dt_ms);
transformf_t scene_timeline_local_at(const scene_t* scene, scene_row_t row, const anim_playhead_t* time,
                                     transformf_t local);
transformf_t scene_timeline_world_at(const scene_t* scene, scene_row_t row, uint32_t ms);
transformf_t scene_row_sample_world(const scene_root_t* root, scene_row_t row, const anim_playhead_t* time);

static inline scene_row_t
scene_asset_parent(const scene_asset_t* asset, scene_row_t row) {
    const uint16_t parent = asset->parents[row];
    return parent == SCENE_ASSET_PARENT_ROOT ? SCENE_ROW_NONE : parent;
}

scene_row_t scene_asset_find_row(const scene_asset_t* asset, scene_row_t first, name_hash_t hash);
scene_failure_t scene_constraint_target(const scene_asset_t* asset, scene_field_mask_t* writes,
                                        const anim_binding_t* binding, name_hash_t hash, scene_row_t* out);
scene_failure_t scene_constraint_resolve(const scene_asset_t* asset, scene_field_mask_t* writes, scene_row_t* targets);
scene_constraint_t* scene_constraint_find(const scene_t* scene, scene_row_t row);
void scene_constraint_open(scene_t* scene);
void scene_constraint_step(scene_t* scene);
/* An animator or light now exists on `entity`: adds it to `staged` once. A
 * skin needs its entity's skeleton animator, so its entity is staged already. */
void scene_stage_add(scene_t* scene, scene_row_t entity);
/* Settles the skins' buffers, then advances and skins. */
void scene_animation_step(scene_t* scene, uint32_t dt_ms);
/* The same without settling: a seek re-poses what is shown and takes no buffers. */
void scene_animation_repose(scene_t* scene, uint32_t dt_ms);

/* Whether `a` picks its pose tier, and so needs its drawn size, at the next
 * step: on the frames its current tier poses it, or before it has a tier. */
static inline bool
scene_animator_due(uint32_t frame, const scene_animator_t* a) {
    return a->interval == 0 || (frame + a->phase) % (uint32_t)a->interval == 0;
}

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

scene_failure_t scene_binding_failure(asset_status_t status, const anim_binding_t* binding);
scene_failure_t scene_binding_name_failure(const anim_binding_t* binding, name_hash_t hash, const char* detail);
scene_failure_t scene_failure_make(scene_status_t status, asset_status_t asset, const char* what);
/* Entity `entity`'s memory for `doing` did not fit when it became ACTIVE: it
 * is not drawn, and scene_failure() names it, until scene_unstarve() as it
 * stops being ACTIVE. */
void scene_starve(scene_t* scene, scene_row_t entity, bool* starved, const char* doing);
void scene_unstarve(scene_t* scene, bool* starved);
/* Stores the failure in `why`, which may be NULL; true for SCENE_OK. */
bool scene_report(scene_failure_t* why, scene_status_t status, asset_status_t asset, const char* what);
