/* scene_format: the SCNE file layout stored in an asset pack.
 * tools/r3d/scene_asset.py writes it; the rows are in scene_internal.h. */
#pragma once

#include "asset/asset_pack.h"

#define SCENE_ASSET                   ASSET_TYPE('S', 'C', 'N', 'E')
#define SCENE_ASSET_VERSION           4U
#define SCENE_LIGHT_KIND_DIRECTIONAL  1U
#define SCENE_ASSET_DEFAULT_CLIP_NONE 0xFFFFU
#define SCENE_ASSET_COLOUR_SET_OWN    0xFFFFU
#define SCENE_ASSET_PARENT_ROOT       0xFFFFU /* an entity_parents row: no parent */
#define SCENE_ASSET_HEADER_SIZE       64U
#define SCENE_ASSET_AT_COUNTS         2U
#define SCENE_ASSET_AT_OFFSETS        24U /* after the counts, padded to 4 */

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
    SCENE_ASSET_PART_COUNT
} scene_asset_part_t;
