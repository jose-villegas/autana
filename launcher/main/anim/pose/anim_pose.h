/* Local joint transforms; caller-owned storage and parents-first model matrices. */
#pragma once
#include "anim/anim_binding.h"
#include "anim/anim_skeleton.h"
#include "math/linear/transformf.h"

typedef struct {
    transformf_t* local;
    uint8_t joint_count;
} anim_pose_t;

typedef enum { ANIM_JOINT_POSITION, ANIM_JOINT_ROTATION, ANIM_JOINT_SCALE } anim_joint_channel_t;

typedef struct {
    anim_track_t curve;
    uint8_t joint;
    anim_joint_channel_t channel;
} anim_joint_bound_t;

void anim_pose_rest(const anim_skeleton_t* skeleton, anim_pose_t* out);
void anim_pose_blend(const anim_pose_t* a, const anim_pose_t* b, float weight, anim_pose_t* out);
void anim_pose_model(const anim_skeleton_t* skeleton, const anim_pose_t* pose, mat4f_t* model);
/* failed follows anim_bind; sample only a successfully bound set, onto a rest pose. */
anim_bind_status_t anim_bind_skeleton(const anim_tracks_t* clip, const anim_skeleton_t* skeleton,
                                      anim_joint_bound_t* out, int count, int* failed);
void anim_sample_pose(const anim_joint_bound_t* bound, int count, float seconds, anim_pose_t* pose);

extern const anim_component_fields_t ANIM_POSE_TRANSFORM_FIELDS;
