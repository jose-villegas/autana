/* scene_asset: mapped SCNE rows and batch views shared by scenes and tools. */
#pragma once

#include "asset/asset_entry.h"
#include "hash/name_hash.h"
#include "math/linear/mat3f.h"
#include "render/batch/r3d_batch.h"

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
    uint8_t kind, mode;
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

r3d_batch_t scene_asset_batch(const scene_asset_t* asset, int row, const r3d_lit_mesh_t* mesh,
                              const r3d_copy_chunk_t* chunks);
