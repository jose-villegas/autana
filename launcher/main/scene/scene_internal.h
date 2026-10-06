/*
 * scene_internal: the one scene allocation's layout, shared by scene.c (load,
 * entities) and scene_draw.c (the active camera and the draw), and the scene
 * entry (SCNE) it is loaded from, read by scene_asset.c. Not for apps.
 */
#pragma once

#include "anim/anim_tracks.h"
#include "scene/scene.h"
#include "scene/scene_shell.h"

#define SCENE_FLAG_ENABLED  1U
#define SCENE_FLAG_DIRTY    2U

/* The scene pack entry, written by tools/r3d/scene_asset.py; the layout is
 * in docs/render/Scene-Files.md. Its rows are read in place, so they are the
 * entry's bytes, little-endian as the device is. */
#define SCENE_ASSET         ASSET_TYPE('S', 'C', 'N', 'E')
#define SCENE_ASSET_VERSION 1U

typedef struct {
    uint16_t entity, pad;
    char mesh[ASSET_NAME_MAX];
} scene_asset_renderer_t;

typedef struct {
    uint16_t entity, pad;
    float half_fov_short_tan, near_z;
    uint32_t clear_rgb;        /* 0xRRGGBB */
    char clip[ASSET_NAME_MAX]; /* empty: the camera has no path */
    char node[ASSET_NAME_MAX]; /* the clip's node that is the camera */
} scene_asset_camera_t;

/* An opened entry. Holds no copy: the pack must outlive it. */
typedef struct {
    uint16_t entity_count, renderer_count, camera_count;
    const char (*names)[ASSET_NAME_MAX];
    const scene_transform_t* transforms;
    const scene_asset_renderer_t* renderers;
    const scene_asset_camera_t* cameras;
} scene_asset_t;

/* Checks every row once and returns the first failure: ASSET_ERR_VERSION,
 * ASSET_ERR_BOUNDS or ASSET_ERR_FORMAT. Else fills `out`. */
asset_status_t scene_asset_open(asset_view_t entry, scene_asset_t* out);

/* A mesh view and the placement built from its entity's transform. */
typedef struct {
    r3d_lit_mesh_t mesh;
    r3d_placement_t placement;
    scene_entity_t entity;
    bool placed; /* false while the transform is the identity: drawn as it is */
} scene_renderer_t;

typedef struct {
    r3d_scene_camera_t lens; /* its path, when it has one, is `path` */
    r3d_scene_path_t path;
    scene_entity_t entity;
    int render_scale_percent;
    uint16_t clear; /* panel pixel format */
} scene_camera_t;

struct scene {
    scene_asset_t asset;
    char id[ASSET_NAME_MAX];
    bool holds_pack; /* loaded by scene_load(): a use of pack `id` */
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

/* The index of the scene's camera `camera` (NULL: its first), or -1. */
int scene_camera_index(const scene_t* scene, const char* camera);

/* A scene is going away: the draw drops it if its camera is active. */
void scene_draw_forget(const scene_t* scene);
void scene_draw_release(void);

/* True when drawing the mesh as it is gives the same picture. */
bool scene_transform_is_identity(const scene_transform_t* transform);
