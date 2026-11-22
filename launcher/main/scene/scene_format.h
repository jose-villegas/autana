/* scene_format: the SCNE file layout stored in an asset pack.
 * tools/r3d/scene_asset.py writes it; the rows are in scene_asset.h. */
#pragma once

#include "asset/asset_pack.h"

#define SCENE_ASSET                           ASSET_TYPE('S', 'C', 'N', 'E')
#define SCENE_ASSET_VERSION                   7U
#define SCENE_LIGHT_KIND_DIRECTIONAL          1U
#define SCENE_ASSET_DEFAULT_CLIP_NONE         0xFFFFU
#define SCENE_ASSET_COLOUR_SET_OWN            0xFFFFU
#define SCENE_ASSET_PARENT_ROOT               0xFFFFU /* an entity_parents row: no parent */
#define SCENE_ASSET_HEADER_SIZE               104U
#define SCENE_ASSET_AT_COUNTS                 2U
#define SCENE_ASSET_AT_OFFSETS                36U /* after the counts, padded to 4 */
/* A names row: the name's name_hash(), then the name, NUL padded. */
#define SCENE_ASSET_NAME_SIZE                 36U
#define SCENE_ASSET_NAME_AT_HASH              0U
#define SCENE_ASSET_NAME_AT_TEXT              4U

#define SCENE_ASSET_TIMELINE_SIZE             36U
#define SCENE_ASSET_TIMELINE_AT_CLIP          0U
#define SCENE_ASSET_TIMELINE_AT_WRAP          32U
#define SCENE_ASSET_TIMELINE_AT_PAD           33U

#define SCENE_ASSET_CONSTRAINT_SIZE           48U
#define SCENE_ASSET_CONSTRAINT_AT_ENTITY      0U
#define SCENE_ASSET_CONSTRAINT_AT_BIND        2U
#define SCENE_ASSET_CONSTRAINT_AT_PAD         3U
#define SCENE_ASSET_CONSTRAINT_AT_FOLLOW      4U
#define SCENE_ASSET_CONSTRAINT_AT_LOOK_AT     8U
#define SCENE_ASSET_CONSTRAINT_AT_OFFSET      12U
#define SCENE_ASSET_CONSTRAINT_AT_LOOK_OFFSET 24U
#define SCENE_ASSET_CONSTRAINT_AT_LAG_MS      36U
#define SCENE_ASSET_CONSTRAINT_AT_HANDOFF_MS  40U
#define SCENE_ASSET_CONSTRAINT_AT_WEIGHT      44U
#define SCENE_ASSET_BIND_WORLD                0U
#define SCENE_ASSET_BIND_TARGET               1U
#define SCENE_ASSET_BIND_TARGET_YAW           2U

#define SCENE_ASSET_HINGE_SIZE                28U
#define SCENE_ASSET_HINGE_AT_ENTITY           0U
#define SCENE_ASSET_HINGE_AT_PAD              2U
#define SCENE_ASSET_HINGE_AT_AXIS             4U
#define SCENE_ASSET_HINGE_AT_MIN              16U
#define SCENE_ASSET_HINGE_AT_MAX              20U
#define SCENE_ASSET_HINGE_AT_ANGLE            24U
#define SCENE_ASSET_HINGE_AXIS_TOLERANCE      0.0001F

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
    SCENE_ASSET_PARENTS, /* u16 per entity: its parent, lower than itself, or SCENE_ASSET_PARENT_ROOT */
    SCENE_ASSET_BATCHES, /* one per renderer that draws copies of its mesh */
    SCENE_ASSET_CHUNKS,  /* each batch's chunks in turn, R3D_BATCH_CHUNK_COPIES copies each */
    SCENE_ASSET_PLACES,  /* each copy's position and colour set, what the cull reads */
    SCENE_ASSET_TURNS,   /* each copy's rotation, read for copies the cull keeps */
    SCENE_ASSET_TIMELINE,
    SCENE_ASSET_CONSTRAINTS,
    SCENE_ASSET_HINGES,
    SCENE_ASSET_PART_COUNT
} scene_asset_part_t;
