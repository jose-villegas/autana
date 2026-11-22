/* Two timed slots during a transition blend; the clip table outlives the layer. */
#pragma once
#include "anim/playback/anim_playhead.h"
#include "anim/pose/anim_pose.h"
#define ANIM_LAYER_CLIP_NONE UINT8_MAX

typedef struct {
    uint8_t clip;
    anim_playhead_t playhead;
} anim_slot_t;

typedef struct {
    anim_slot_t current, previous;
    const anim_clip_t* clips;
    int clip_count;
    uint32_t blend_ms, blended_ms;
    anim_pose_t shown, frozen;
    bool frozen_previous;
} anim_layer_t;

typedef struct {
    const anim_track_t *clip, *speed, *blend;
    const uint8_t* slots;
} anim_blocks_t;

typedef struct {
    anim_slot_t current, previous;
    name_hash_t current_name, previous_name;
    uint32_t blend_ms, blended_ms;
} anim_block_sample_t;

/* seconds is the timeline sample time for t_ms, including its clamped endpoint. */
anim_block_sample_t anim_blocks_sample(const anim_blocks_t* blocks, const anim_clip_t* clips, anim_wrap_t wrap,
                                       uint32_t t_ms, float seconds);
void anim_layer_set(anim_layer_t* layer, const anim_block_sample_t* sample);

/* frozen.local must not alias shown.local; both poses have the skeleton's joint count. Zero joints need no storage. */
void anim_layer_init(anim_layer_t* layer, const anim_clip_t* clips, int count, anim_pose_t shown, anim_pose_t frozen);
/* An interrupted transition blend freezes the last displayed pose; a first play starts immediately. */
void anim_layer_play(anim_layer_t* layer, uint8_t clip, uint32_t blend_ms);
void anim_layer_advance(anim_layer_t* layer, uint32_t dt_ms);
float anim_layer_weight(const anim_layer_t* layer);

/* Bound tables follow the clip table. The returned shown view is valid until the next pose call. */
const anim_pose_t* anim_layer_pose(anim_layer_t* layer, const anim_skeleton_t* skeleton,
                                   const anim_joint_bound_t* const* bound, const int* counts);
