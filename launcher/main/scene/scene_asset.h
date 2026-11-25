/* scene_asset: mapped SCNE rows and batch views shared by scenes and tools. */
#pragma once

#include <stddef.h>

#include "asset/asset_entry.h"
#include "hash/name_hash.h"
#include "math/linear/mat3f.h"
#include "math/linear/quatf.h"
#include "render/batch/r3d_batch.h"
#include "scene/scene_format.h"

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

/* Pin each mapped field's offset to scene_format.h. */
#define SCNE_FIELD_AT(row, field, offset) (offsetof(row, field) == SCENE_ASSET_##offset)
#define SCNE_HINGE_AT(field, offset)      SCNE_FIELD_AT(scene_asset_hinge_t, field, HINGE_AT_##offset)

typedef struct {
    char clip[ASSET_NAME_MAX];
    uint8_t wrap, pad[SCENE_ASSET_TIMELINE_PAD_BYTES];
} scene_asset_timeline_t;

_Static_assert(sizeof(scene_asset_timeline_t) == SCENE_ASSET_TIMELINE_SIZE
                   && SCNE_FIELD_AT(scene_asset_timeline_t, clip, TIMELINE_AT_CLIP)
                   && SCNE_FIELD_AT(scene_asset_timeline_t, wrap, TIMELINE_AT_WRAP)
                   && SCNE_FIELD_AT(scene_asset_timeline_t, pad, TIMELINE_AT_PAD),
               "SCNE timeline row layout");

#define SCNE_CONSTRAINT_COMMON_LAYOUT(row, prefix)                                                                     \
    (sizeof(row) == SCENE_ASSET_##prefix##_SIZE && SCNE_FIELD_AT(row, entity, prefix##_AT_ENTITY)                      \
     && SCNE_FIELD_AT(row, source_count, prefix##_AT_SOURCE_COUNT)                                                     \
     && SCNE_FIELD_AT(row, source_first, prefix##_AT_SOURCE_FIRST) && SCNE_FIELD_AT(row, weight, prefix##_AT_WEIGHT))

typedef struct {
    uint16_t entity;
    uint8_t binding_mode, source_count;
    uint16_t source_first, pad;
    float weight;
    vec3f_t translation_offset;
} scene_asset_position_constraint_t;

_Static_assert(SCNE_CONSTRAINT_COMMON_LAYOUT(scene_asset_position_constraint_t, POSITION_CONSTRAINT)
                   && SCNE_FIELD_AT(scene_asset_position_constraint_t, binding_mode,
                                    POSITION_CONSTRAINT_AT_BINDING_MODE)
                   && SCNE_FIELD_AT(scene_asset_position_constraint_t, pad, POSITION_CONSTRAINT_AT_PAD)
                   && SCNE_FIELD_AT(scene_asset_position_constraint_t, translation_offset,
                                    POSITION_CONSTRAINT_AT_TRANSLATION_OFFSET),
               "SCNE position constraint row layout");

typedef struct {
    uint16_t entity;
    uint8_t world_up_type, source_count;
    uint16_t source_first, world_up_object;
    float weight;
    vec3f_t aim_vector, up_vector, world_up_vector;
    quatf_t rotation_offset;
} scene_asset_aim_constraint_t;

_Static_assert(SCNE_CONSTRAINT_COMMON_LAYOUT(scene_asset_aim_constraint_t, AIM_CONSTRAINT)
                   && SCNE_FIELD_AT(scene_asset_aim_constraint_t, world_up_type, AIM_CONSTRAINT_AT_WORLD_UP_TYPE)
                   && SCNE_FIELD_AT(scene_asset_aim_constraint_t, world_up_object, AIM_CONSTRAINT_AT_WORLD_UP_OBJECT)
                   && SCNE_FIELD_AT(scene_asset_aim_constraint_t, aim_vector, AIM_CONSTRAINT_AT_AIM_VECTOR)
                   && SCNE_FIELD_AT(scene_asset_aim_constraint_t, up_vector, AIM_CONSTRAINT_AT_UP_VECTOR)
                   && SCNE_FIELD_AT(scene_asset_aim_constraint_t, world_up_vector, AIM_CONSTRAINT_AT_WORLD_UP_VECTOR)
                   && SCNE_FIELD_AT(scene_asset_aim_constraint_t, rotation_offset, AIM_CONSTRAINT_AT_ROTATION_OFFSET),
               "SCNE aim constraint row layout");

typedef struct {
    name_hash_t target;
    float weight;
} scene_asset_constraint_source_t;

static inline bool
scene_constraint_weight_fits(float weight) {
    return weight >= 0.0F && weight <= 1.0F;
}

_Static_assert(sizeof(scene_asset_constraint_source_t) == SCENE_ASSET_CONSTRAINT_SOURCE_SIZE
                   && SCNE_FIELD_AT(scene_asset_constraint_source_t, target, CONSTRAINT_SOURCE_AT_TARGET)
                   && SCNE_FIELD_AT(scene_asset_constraint_source_t, weight, CONSTRAINT_SOURCE_AT_WEIGHT),
               "SCNE constraint source row layout");

typedef struct {
    uint16_t entity, pad;
    vec3f_t axis;
    float min, max, angle;
} scene_asset_hinge_t;

_Static_assert(sizeof(scene_asset_hinge_t) == SCENE_ASSET_HINGE_SIZE && SCNE_HINGE_AT(entity, ENTITY)
                   && SCNE_HINGE_AT(pad, PAD) && SCNE_HINGE_AT(axis, AXIS) && SCNE_HINGE_AT(min, MIN)
                   && SCNE_HINGE_AT(max, MAX) && SCNE_HINGE_AT(angle, ANGLE),
               "SCNE hinge row layout");

/* An opened entry. Holds no copy: the pack must outlive it. */
typedef struct {
    uint16_t entity_count, renderer_count, camera_count;
    uint16_t light_count, skinned_renderer_count, animator_count, clip_count;
    uint16_t batch_count, chunk_count, copy_count;
    uint16_t timeline_count, position_constraint_count, aim_constraint_count, source_count, hinge_count;
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
    const scene_asset_timeline_t* timeline;
    const scene_asset_position_constraint_t* position_constraints;
    const scene_asset_aim_constraint_t* aim_constraints;
    const scene_asset_constraint_source_t* sources;
    const scene_asset_hinge_t* hinges;
} scene_asset_t;

/* Checks every row once and returns the first failure: ASSET_ERR_VERSION,
 * ASSET_ERR_BOUNDS or ASSET_ERR_FORMAT. Else fills `out`. */
asset_status_t scene_asset_open(asset_view_t entry, scene_asset_t* out);

r3d_batch_t scene_asset_batch(const scene_asset_t* asset, int row, const r3d_lit_mesh_t* mesh,
                              const r3d_copy_chunk_t* chunks);
