#include "anim/pose/anim_pose.h"
#include <assert.h>
#include <string.h>
#include "anim/anim_skeleton_format.h"

static const anim_field_t FIELDS[] = {
    ANIM_FIELD(transformf_t, position, ANIM_VALUE_VEC3),
    ANIM_FIELD(transformf_t, rotation, ANIM_VALUE_QUAT),
    ANIM_FIELD(transformf_t, scale, ANIM_VALUE_VEC3),
};
const anim_component_fields_t ANIM_POSE_TRANSFORM_FIELDS = {ANIM_COMPONENT_TRANSFORM, FIELDS,
                                                            sizeof FIELDS / sizeof FIELDS[0]};

void
anim_pose_rest(const anim_skeleton_t* skeleton, anim_pose_t* out) {
    assert(out->joint_count == skeleton->joint_count);
    for (int j = 0; j < skeleton->joint_count; j++) {
        const float* rest = skeleton->rest[j];
        transformf_set_position(&out->local[j], (vec3f_t){rest[0], rest[1], rest[2]});
        transformf_set_rotation(&out->local[j], (quatf_t){rest[3], rest[4], rest[5], rest[6]});
        transformf_set_scale(&out->local[j], (vec3f_t){rest[7], rest[8], rest[9]});
    }
}

void
anim_pose_blend(const anim_pose_t* a, const anim_pose_t* b, float weight, anim_pose_t* out) {
    assert(a->joint_count == b->joint_count && a->joint_count == out->joint_count);
    const float to = mathf_clamp(weight, 0.0F, 1.0F);
    const float from = 1.0F - to;
    for (int j = 0; j < out->joint_count; j++) {
        const transformf_t av = a->local[j];
        const transformf_t bv = b->local[j];
        transformf_set_position(&out->local[j],
                                vec3f_add(vec3f_scale(av.position, from), vec3f_scale(bv.position, to)));
        transformf_set_scale(&out->local[j], vec3f_add(vec3f_scale(av.scale, from), vec3f_scale(bv.scale, to)));
        transformf_set_rotation(&out->local[j], quatf_nlerp(av.rotation, bv.rotation, to));
    }
}

void
anim_pose_model(const anim_skeleton_t* skeleton, const anim_pose_t* pose, mat4f_t* model) {
    assert(pose->joint_count == skeleton->joint_count);
    for (int j = 0; j < pose->joint_count; j++) {
        const mat4f_t local = transformf_compute_matrix(&pose->local[j]);
        const int parent = skeleton->joints[j].parent;
        model[j] = parent == ANIM_SKELETON_ROOT ? local : mat4f_mul(model[parent], local);
    }
}

static int
find_joint(const anim_skeleton_t* skeleton, const char* path) {
    for (int j = 0; j < skeleton->joint_count; j++) {
        if (strcmp(skeleton->paths + skeleton->joints[j].path, path) == 0) {
            return j;
        }
    }
    return -1;
}

static anim_bind_status_t
bind_joint(const anim_binding_t* binding, int joint, anim_joint_bound_t* out) {
    if (joint < 0) {
        return ANIM_BIND_ERR_PATH;
    }
    transformf_t storage;
    const anim_component_ref_t component = {&ANIM_POSE_TRANSFORM_FIELDS, &storage};
    anim_bound_t resolved;
    const anim_bind_status_t status = anim_bind_field(binding, &component, 1, NULL, 0, &resolved);
    if (status != ANIM_BIND_OK) {
        return status;
    }
    const size_t offset = (size_t)((uint8_t*)resolved.target - (uint8_t*)&storage);
    for (int c = ANIM_JOINT_POSITION; c <= ANIM_JOINT_SCALE; c++) {
        if (offset == FIELDS[c].offset) {
            *out = (anim_joint_bound_t){resolved.curve, (uint8_t)joint, (anim_joint_channel_t)c};
            return ANIM_BIND_OK;
        }
    }
    assert(false);
    return ANIM_BIND_ERR_FIELD;
}

anim_bind_status_t
anim_bind_skeleton(const anim_tracks_t* clip, const anim_skeleton_t* skeleton, anim_joint_bound_t* out, int count,
                   int* failed) {
    if (failed != NULL) {
        *failed = -1;
    }
    if (clip->root != ANIM_ROOT_SKELETON || count < clip->count) {
        return clip->root == ANIM_ROOT_SKELETON ? ANIM_BIND_ERR_SPACE : ANIM_BIND_ERR_ROOT;
    }
    for (int i = 0; i < clip->count; i++) {
        anim_binding_t binding;
        (void)anim_tracks_binding_at(clip, i, &binding);
        const anim_bind_status_t status = bind_joint(&binding, find_joint(skeleton, binding.path), &out[i]);
        if (status != ANIM_BIND_OK) {
            if (failed != NULL) {
                *failed = i;
            }
            return status;
        }
    }
    return ANIM_BIND_OK;
}

void
anim_sample_pose(const anim_joint_bound_t* bound, int count, float seconds, anim_pose_t* pose) {
    for (int i = 0; i < count; i++) {
        const anim_joint_bound_t* b = &bound[i];
        assert(b->joint < pose->joint_count);
        float value[ANIM_WIDTH_MAX];
        anim_track_sample(&b->curve, seconds, value);
        transformf_t* local = &pose->local[b->joint];
        switch (b->channel) {
            case ANIM_JOINT_POSITION: transformf_set_position(local, (vec3f_t){value[0], value[1], value[2]}); break;
            case ANIM_JOINT_ROTATION:
                transformf_set_rotation(local, (quatf_t){value[0], value[1], value[2], value[3]});
                break;
            case ANIM_JOINT_SCALE: transformf_set_scale(local, (vec3f_t){value[0], value[1], value[2]}); break;
        }
    }
}
