/* anim_skeleton_format: the SKEL file layout stored in an asset pack. */
#pragma once

#include "asset/asset_pack.h"

#define ANIM_SKELETON_ASSET           ASSET_TYPE('S', 'K', 'E', 'L')
#define ANIM_SKELETON_VERSION         1U
#define ANIM_SKELETON_UNIT_TOLERANCE  1e-4F
#define ANIM_SKELETON_ROOT            255U
#define ANIM_SKELETON_JOINT_MAX       254U

#define ANIM_SKELETON_HEADER_SIZE     16U
#define ANIM_SKELETON_AT_VERSION      0U
#define ANIM_SKELETON_AT_COUNT        2U
#define ANIM_SKELETON_AT_PAD          3U
#define ANIM_SKELETON_AT_STRINGS      4U
#define ANIM_SKELETON_AT_STRINGS_SIZE 8U
#define ANIM_SKELETON_AT_REST         12U
#define ANIM_SKELETON_ALIGNMENT       4U
#define ANIM_SKELETON_ROW_SIZE        4U
#define ANIM_SKELETON_REST_WIDTH      10U
#define ANIM_SKELETON_ROTATION        3U
#define ANIM_SKELETON_ROTATION_WIDTH  4U
#define ANIM_SKELETON_STRING_MAX      65535U
