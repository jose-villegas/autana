/*
 * scene_internal: the scene's layout and components, shared by scene.c
 * (load, entities), scene_animation.c (animators, skinning) and scene_draw.c
 * (the active camera and the draw), and the SCNE entry scene_asset.c reads.
 * Not for apps.
 */
#pragma once

#include "anim/anim_tracks.h"
#include "anim/playback/anim_layer.h"
#include "core/memory.h"
#include "math/linear/mat3f.h"
#include "render/skin/r3d_skin_light.h"
#include "scene/scene.h"
#include "scene/scene_shell.h"

/* Absent ambient contributes zero. */
#define SCENE_AMBIENT_INTENSITY_NONE  0.0F
#define SCENE_TONEMAP_WHITE_NONE      0.0F
#define SCENE_LIGHT_INTENSITY_DEFAULT 0.0F
#define SCENE_LIGHT_COLOR_DEFAULT     ((vec3f_t){1.0F, 1.0F, 1.0F})
#define SCENE_LIGHT_AXIS              ((vec3f_t){0.0F, 1.0F, 0.0F})

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
#define SCENE_LIGHT_FAILURE_ALBEDO "skin vertex colors required"
#define SCENE_LIGHT_FAILURE_NORMAL "skin normal transform invalid"

typedef struct {
    mat3f_t linear;
    vec3f_t position;
} scene_asset_transform_t;

/* The scene pack entry, written by tools/r3d/scene_asset.py; the layout is
 * in docs/render/Scene-Files.md. Its rows are read in place, so they are the
 * entry's bytes, little-endian as the device is. */
#define SCENE_ASSET                   ASSET_TYPE('S', 'C', 'N', 'E')
#define SCENE_ASSET_VERSION           2U
#define SCENE_LIGHT_KIND_DIRECTIONAL  1U
#define SCENE_ASSET_DEFAULT_CLIP_NONE 0xFFFFU
#define SCENE_ASSET_HEADER_SIZE       56U
#define SCENE_ASSET_AT_COUNTS         2U
#define SCENE_ASSET_AT_OFFSETS        20U

typedef enum {
    SCENE_ASSET_NAMES,
    SCENE_ASSET_TRANSFORMS,
    SCENE_ASSET_RENDERERS,
    SCENE_ASSET_CAMERAS,
    SCENE_ASSET_LIGHTS,
    SCENE_ASSET_SKINS,
    SCENE_ASSET_ANIMATORS,
    SCENE_ASSET_CLIPS,
    SCENE_ASSET_SETTINGS,
    SCENE_ASSET_PART_COUNT
} scene_asset_part_t;

typedef struct {
    uint16_t entity, pad;
    char mesh[ASSET_NAME_MAX];
} scene_asset_renderer_t;

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
    const char (*names)[ASSET_NAME_MAX];
    const scene_asset_transform_t* transforms;
    const scene_asset_renderer_t* renderers;
    const scene_asset_camera_t* cameras;
    const scene_asset_light_t* lights;
    const scene_asset_skinned_renderer_t* skinned_renderers;
    const scene_asset_animator_t* animators;
    const char (*clips)[ASSET_NAME_MAX];
    const scene_asset_settings_t* settings;
} scene_asset_t;

/* Checks every row once and returns the first failure: ASSET_ERR_VERSION,
 * ASSET_ERR_BOUNDS or ASSET_ERR_FORMAT. Else fills `out`. */
asset_status_t scene_asset_open(asset_view_t entry, scene_asset_t* out);

/* A mesh view and the model matrix built from its entity's transform. */
typedef struct {
    r3d_lit_mesh_t mesh;
    mat4f_t model;
    scene_entity_t entity;
    bool placed; /* false while the transform is the identity: drawn as it is */
} scene_renderer_t;

typedef struct {
    camera_t lens;
    scene_entity_t entity;
    uint16_t clear; /* panel pixel format */
} scene_camera_t;

typedef struct {
    anim_tracks_t tracks;
    const char* id;
    anim_joint_bound_t* joints;
    anim_bound_t* fields;
} scene_animator_clip_t;

typedef struct {
    anim_layer_t layer;
    anim_skeleton_t skeleton;
    scene_animator_clip_t* clips;
    void* storage;
    const anim_joint_bound_t** joint_tables;
    int* counts;
    uint8_t root;
    anim_wrap_t wrap;
    bool dirty, posed;
} scene_animator_t;

typedef enum {
    SCENE_SKIN_LIGHT_DIRECT = R3D_SKIN_LIGHT_DIRECT,
    SCENE_SKIN_LIGHT_RENORMALISED = R3D_SKIN_LIGHT_RENORMALISED,
    SCENE_SKIN_LIGHT_TABLE = R3D_SKIN_LIGHT_TABLE
} scene_skin_light_mode_t;

typedef struct {
    r3d_skin_t skin;
    r3d_lit_mesh_t bind;
    r3d_skin_output_t output;
    mat4f_t* model;
    mat4f_t* palette;
    void* internal_storage;
    uint8_t (*colors)[R3D_SKIN_LIGHT_CHANNELS];
    mat3f_t* normal_palette;
    void* light_storage;
    bool lighting_valid;
    scene_skin_light_mode_t light_mode;
} scene_skinned_renderer_t;

struct scene {
    scene_asset_t asset;
    char id[ASSET_NAME_MAX];
    bool holds_pack; /* loaded by scene_load(): a use of pack `id` */
    int renderer_count;
    int animator_count, skeleton_animator_count;
    int light_count;
    r3d_skin_lights_t world_lights;
    scene_light_t* lights;
    vec3f_t ambient_color;
    float ambient_intensity, tonemap_white;
    r3d_skin_light_transfer_t* light_transfer;
    r3d_skin_light_table_t* light_table;
    int light_table_users;
    bool light_table_dirty;
    uint8_t* flags; /* one per entity: SCENE_FLAG_* */
    scene_renderer_t* renderers;
    scene_camera_t* cameras;
    const asset_pack_t* pack;
    transformf_t* locals;
    scene_animator_t* animators;
    scene_skinned_renderer_t* skinned_renderers;
    r3d_instance_t* instances; /* one per renderer: what the raster draws this frame */
};

#if !defined(ESP_PLATFORM) || CONFIG_LAUNCHER_DEVELOPMENT
/* Fault injection exercises allocation rollback without exhausting the heap. */
extern int scene_fail_allocation_after;
#endif

void* scene_allocate(size_t bytes, memory_kind_t kind);

/* The loaded scenes, in load order. */
int scene_loaded_count(void);
scene_t* scene_loaded_at(int index);

/* The index of the scene's camera `camera` (NULL: its first), or -1. */
int scene_camera_index(const scene_t* scene, const char* camera);

/* A scene is going away: the draw drops it if its camera is active. */
void scene_draw_forget(const scene_t* scene);
void scene_draw_release(void);

/* True when drawing the mesh as it is gives the same picture. */
bool scene_transform_is_identity(const mat4f_t* transform);

size_t scene_storage_carve(size_t* end, size_t bytes, size_t align);
#define SCENE_STORAGE_RESERVE(layout, end, member, type, count)                                                        \
    ((layout).member = scene_storage_carve(&(end), sizeof(type) * (size_t)(count), _Alignof(type)))
int scene_renderer_index(const scene_t* scene, scene_entity_t entity);
void scene_animation_release(scene_t* scene);
void scene_animation_step(scene_t* scene, uint32_t dt_ms);
void scene_animate_range(scene_t* scene, int first, int count, uint32_t dt_ms);
bool scene_skin_light_set_mode(scene_t* scene, scene_entity_t entity, scene_skin_light_mode_t mode);
bool scene_skin_light_apply_mode(scene_t* scene, scene_entity_t entity, scene_skinned_renderer_t* skin,
                                 scene_skin_light_mode_t mode);
void scene_skin_light_table_release(scene_t* scene, scene_entity_t entity);
bool scene_skin_light_allocate(scene_skinned_renderer_t* skin);
void scene_skin_light_prepare(scene_t* scene, scene_entity_t entity);
bool scene_light_color_fits(vec3f_t linear_rgb);
r3d_skin_light_transfer_t* scene_light_transfer_take(const scene_t* scene);
bool scene_lights_allocate(scene_t* scene);
void scene_lights_prepare(scene_t* scene);
void scene_skin_range(scene_t* scene, int first, int count);

scene_failure_t scene_failure_make(scene_status_t status, asset_status_t asset, const char* what);
