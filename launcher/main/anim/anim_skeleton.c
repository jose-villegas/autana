#include "anim/anim_skeleton.h"

#include <math.h>
#include <stddef.h>
#include <string.h>
#include "asset/asset_bytes.h"
#include "asset/asset_entry.h"

_Static_assert(sizeof(anim_skeleton_joint_t) == ANIM_SKELETON_ROW_SIZE, "SKEL joint layout");

asset_status_t
anim_skeleton_open(const asset_pack_t* pack, const char* id, anim_skeleton_t* out) {
    *out = (anim_skeleton_t){0};
    asset_view_t entry;
    asset_status_t status =
        asset_entry_open(pack, id, ANIM_SKELETON_ASSET, ANIM_SKELETON_VERSION, ANIM_SKELETON_AT_VERSION,
                         ANIM_SKELETON_HEADER_SIZE, ANIM_SKELETON_ALIGNMENT, &entry);
    if (status != ASSET_OK) {
        return status;
    }
    const uint8_t count = entry.data[ANIM_SKELETON_AT_COUNT];
    if (count == 0 || count > ANIM_SKELETON_JOINT_MAX || entry.data[ANIM_SKELETON_AT_PAD] != 0) {
        return ASSET_ERR_FORMAT;
    }
    const uint32_t strings = asset_read_u32(entry.data + ANIM_SKELETON_AT_STRINGS);
    const uint32_t strings_size = asset_read_u32(entry.data + ANIM_SKELETON_AT_STRINGS_SIZE);
    const uint32_t rest = asset_read_u32(entry.data + ANIM_SKELETON_AT_REST);
    const uint32_t table_end = ANIM_SKELETON_HEADER_SIZE + count * ANIM_SKELETON_ROW_SIZE;
    const uint64_t rest_end = (uint64_t)rest + count * ANIM_SKELETON_REST_WIDTH * sizeof(float);
    if (rest % ANIM_SKELETON_ALIGNMENT != 0 || strings % ANIM_SKELETON_ALIGNMENT != 0 || rest < table_end
        || strings < rest_end || (uint64_t)strings + strings_size != entry.size
        || strings_size > ANIM_SKELETON_STRING_MAX) {
        return ASSET_ERR_BOUNDS;
    }
    if (!asset_entry_zero(entry, table_end, rest) || !asset_entry_zero(entry, (uint32_t)rest_end, strings)) {
        return ASSET_ERR_FORMAT;
    }
    const anim_skeleton_t skeleton = {
        .joint_count = count,
        .joints = (const anim_skeleton_joint_t*)(const void*)(entry.data + ANIM_SKELETON_HEADER_SIZE),
        .rest = (const float(*)[ANIM_SKELETON_REST_WIDTH])(const void*)(entry.data + rest),
        .paths = (const char*)entry.data + strings,
    };
    for (int i = 0; i < count; i++) {
        const anim_skeleton_joint_t joint = skeleton.joints[i];
        if (joint.pad != 0 || (joint.parent != ANIM_SKELETON_ROOT && joint.parent >= i) || joint.path >= strings_size
            || skeleton.paths[joint.path] == 0 || (joint.path > 0 && skeleton.paths[joint.path - 1] != 0)
            || memchr(skeleton.paths + joint.path, 0, strings_size - joint.path) == NULL) {
            return ASSET_ERR_FORMAT;
        }
        for (int j = 0; j < i; j++) {
            if (strcmp(skeleton.paths + joint.path, skeleton.paths + skeleton.joints[j].path) == 0) {
                return ASSET_ERR_FORMAT;
            }
        }
        for (int k = 0; k < ANIM_SKELETON_REST_WIDTH; k++) {
            if (!isfinite(skeleton.rest[i][k])) {
                return ASSET_ERR_FORMAT;
            }
        }
        double norm = 0;
        for (int k = ANIM_SKELETON_ROTATION; k < ANIM_SKELETON_ROTATION + ANIM_SKELETON_ROTATION_WIDTH; k++) {
            const double q = skeleton.rest[i][k];
            norm += q * q;
        }
        if (fabs(1.0 - norm) > ANIM_SKELETON_UNIT_TOLERANCE) {
            return ASSET_ERR_FORMAT;
        }
    }
    *out = skeleton;
    return ASSET_OK;
}
