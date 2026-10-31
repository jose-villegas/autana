/*
 * anim_skeleton: a validated SKEL v1 mapped view; the pack outlives it.
 * docs/assets/formats/SKEL.md owns the layout. No allocation.
 */
#pragma once
#include <stdint.h>
#include "anim/anim_skeleton_format.h"
#include "asset/asset_pack.h"

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
