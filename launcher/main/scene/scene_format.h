/* scene_format: the SCNE file layout stored in an asset pack.
 * tools/r3d/scene_asset.py writes it; the rows are in scene_internal.h. */
#pragma once

#include "asset/asset_pack.h"

#define SCENE_ASSET                   ASSET_TYPE('S', 'C', 'N', 'E')
#define SCENE_ASSET_VERSION           6U
#define SCENE_LIGHT_KIND_DIRECTIONAL  1U
#define SCENE_ASSET_DEFAULT_CLIP_NONE 0xFFFFU
#define SCENE_ASSET_COLOUR_SET_OWN    0xFFFFU
#define SCENE_ASSET_PARENT_ROOT       0xFFFFU /* an entity_parents row: no parent */
#define SCENE_ASSET_HEADER_SIZE       88U
#define SCENE_ASSET_AT_COUNTS         2U
#define SCENE_ASSET_AT_OFFSETS        32U /* after the counts, padded to 4 */
/* A names row: the name's name_hash(), then the name, NUL padded. */
#define SCENE_ASSET_NAME_SIZE         36U
#define SCENE_ASSET_NAME_AT_HASH      0U
#define SCENE_ASSET_NAME_AT_TEXT      4U

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
    SCENE_ASSET_PART_COUNT
} scene_asset_part_t;
