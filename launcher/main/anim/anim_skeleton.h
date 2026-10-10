/*
 * anim_skeleton: a validated SKEL v1 mapped view; the pack outlives it.
 * docs/render/Skeleton-and-Skin.md owns the layout. No allocation.
 */
#pragma once
#include <stdint.h>
#include "asset/asset_pack.h"

#define ANIM_SKELETON_ASSET          ASSET_TYPE('S', 'K', 'E', 'L')
#define ANIM_SKELETON_VERSION        1U
#define ANIM_SKELETON_UNIT_TOLERANCE 1e-4F
#define ANIM_SKELETON_ROOT           255U
#define ANIM_SKELETON_JOINT_MAX      254U

enum {
    ANIM_SKELETON_HEADER_SIZE = 16,
    ANIM_SKELETON_AT_VERSION = 0,
    ANIM_SKELETON_AT_COUNT = 2,
    ANIM_SKELETON_AT_PAD = 3,
    ANIM_SKELETON_AT_STRINGS = 4,
    ANIM_SKELETON_AT_STRINGS_SIZE = 8,
    ANIM_SKELETON_AT_REST = 12,
    ANIM_SKELETON_ALIGNMENT = 4,
    ANIM_SKELETON_ROW_SIZE = 4,
    ANIM_SKELETON_REST_WIDTH = 10,
    ANIM_SKELETON_ROTATION = 3,
    ANIM_SKELETON_ROTATION_WIDTH = 4,
    ANIM_SKELETON_STRING_MAX = 65535,
};

typedef struct {
    uint16_t path;
    uint8_t parent;
    uint8_t pad;
} anim_skeleton_joint_t;

typedef struct {
    uint8_t joint_count;
    const anim_skeleton_joint_t* joints;
    const float (*rest)[ANIM_SKELETON_REST_WIDTH];
    const char* paths;
} anim_skeleton_t;

asset_status_t anim_skeleton_open(const asset_pack_t* pack, const char* id, anim_skeleton_t* out);
