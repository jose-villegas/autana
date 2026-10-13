#include "anim/anim_skeleton.h"

#include <math.h>
#include <stddef.h>
#include <string.h>
#include "asset/asset_bytes.h"
#include "asset/asset_entry.h"

_Static_assert(sizeof(anim_skeleton_joint_t) == ANIM_SKELETON_ROW_SIZE, "SKEL joint layout");

static asset_status_t
read_header(asset_view_t entry, anim_skeleton_t* out, uint32_t* out_strings_size) {
    const uint8_t count = entry.data[ANIM_SKELETON_AT_COUNT];
    if (count == 0 || count > ANIM_SKELETON_JOINT_MAX || entry.data[ANIM_SKELETON_AT_PAD] != 0) {
        return ASSET_ERR_FORMAT;
    }
    const uint32_t strings = asset_read_u32(entry.data + ANIM_SKELETON_AT_STRINGS);
    const uint32_t strings_size = asset_read_u32(entry.data + ANIM_SKELETON_AT_STRINGS_SIZE);
    const uint32_t rest = asset_read_u32(entry.data + ANIM_SKELETON_AT_REST);
    const uint32_t table_end = ANIM_SKELETON_HEADER_SIZE + (count * ANIM_SKELETON_ROW_SIZE);
    const uint64_t rest_end = (uint64_t)rest + (count * ANIM_SKELETON_REST_WIDTH * sizeof(float));
    if (rest % ANIM_SKELETON_ALIGNMENT != 0 || strings % ANIM_SKELETON_ALIGNMENT != 0 || rest < table_end
        || strings < rest_end || (uint64_t)strings + strings_size != entry.size
        || strings_size > ANIM_SKELETON_STRING_MAX) {
        return ASSET_ERR_BOUNDS;
    }
    if (!asset_entry_zero(entry, table_end, rest) || !asset_entry_zero(entry, (uint32_t)rest_end, strings)) {
        return ASSET_ERR_FORMAT;
    }
    *out = (anim_skeleton_t){
        .joint_count = count,
        .joints = (const anim_skeleton_joint_t*)(const void*)(entry.data + ANIM_SKELETON_HEADER_SIZE),
        .rest = (const float(*)[ANIM_SKELETON_REST_WIDTH])(const void*)(entry.data + rest),
        .paths = (const char*)entry.data + strings,
    };
    *out_strings_size = strings_size;
    return ASSET_OK;
}

static asset_status_t
check_joint(const anim_skeleton_t* skeleton, int i, uint32_t strings_size) {
    const anim_skeleton_joint_t joint = skeleton->joints[i];
    if (joint.pad != 0 || (joint.parent != ANIM_SKELETON_ROOT && joint.parent >= i) || joint.path >= strings_size
        || skeleton->paths[joint.path] == 0 || (joint.path > 0 && skeleton->paths[joint.path - 1] != 0)
        || memchr(skeleton->paths + joint.path, 0, strings_size - joint.path) == NULL) {
        return ASSET_ERR_FORMAT;
    }
    for (int j = 0; j < i; j++) {
        if (strcmp(skeleton->paths + joint.path, skeleton->paths + skeleton->joints[j].path) == 0) {
            return ASSET_ERR_FORMAT;
        }
    }
    return ASSET_OK;
}

static asset_status_t
check_rest(const float* rest) {
    for (int k = 0; k < ANIM_SKELETON_REST_WIDTH; k++) {
        if (!isfinite(rest[k])) {
            return ASSET_ERR_FORMAT;
        }
    }
    float norm = 0.0F;
    for (int k = ANIM_SKELETON_ROTATION; k < ANIM_SKELETON_ROTATION + ANIM_SKELETON_ROTATION_WIDTH; k++) {
        const float q = rest[k];
        norm += q * q;
    }
    if (fabsf(1.0F - norm) > ANIM_SKELETON_UNIT_TOLERANCE) {
        return ASSET_ERR_FORMAT;
    }
    return ASSET_OK;
}

static asset_status_t
check_rows(const anim_skeleton_t* skeleton, uint32_t strings_size) {
    for (int i = 0; i < skeleton->joint_count; i++) {
        asset_status_t status = check_joint(skeleton, i, strings_size);
        if (status != ASSET_OK) {
            return status;
        }
        status = check_rest(skeleton->rest[i]);
        if (status != ASSET_OK) {
            return status;
        }
    }
    return ASSET_OK;
}

/* The header, then every row, of an opened entry. */
static asset_status_t
parse(asset_view_t entry, void* out) {
    anim_skeleton_t* skeleton = out;
    uint32_t strings_size;
    const asset_status_t status = read_header(entry, skeleton, &strings_size);
    return status == ASSET_OK ? check_rows(skeleton, strings_size) : status;
}

static const asset_entry_format_t FORMAT = {ANIM_SKELETON_ASSET, ANIM_SKELETON_VERSION, ANIM_SKELETON_AT_VERSION,
                                            ANIM_SKELETON_HEADER_SIZE, ANIM_SKELETON_ALIGNMENT};

asset_status_t
anim_skeleton_open(const asset_pack_t* pack, const char* id, anim_skeleton_t* out) {
    return asset_entry_read(pack, id, &FORMAT, parse, out, sizeof *out);
}
