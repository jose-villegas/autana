#include "render/r3d_skin.h"

#include <math.h>
#include <stddef.h>
#include <string.h>
#include "anim/anim_skeleton.h"
#include "asset/asset_bytes.h"
#include "asset/asset_entry.h"

static asset_status_t
read_header(asset_view_t entry, r3d_skin_t* out) {
    const uint8_t joints = entry.data[R3D_SKIN_AT_JOINTS];
    const uint8_t influences = entry.data[R3D_SKIN_AT_INFLUENCES];
    if (joints == 0 || joints > ANIM_SKELETON_JOINT_MAX
        || (influences != R3D_SKIN_INFLUENCES_MIN && influences != R3D_SKIN_INFLUENCES_MAX)) {
        return ASSET_ERR_FORMAT;
    }
    const uint32_t vertices = asset_read_u32(entry.data + R3D_SKIN_AT_VERTICES);
    const uint32_t inverse = asset_read_u32(entry.data + R3D_SKIN_AT_INVERSE);
    const uint32_t records = asset_read_u32(entry.data + R3D_SKIN_AT_RECORDS);
    const uint8_t stride = influences * 2 + R3D_SKIN_RECORD_TAIL;
    const uint64_t inverse_end = (uint64_t)inverse + joints * R3D_SKIN_MATRIX_WIDTH * sizeof(float);
    if (inverse % R3D_SKIN_ALIGNMENT != 0 || records % R3D_SKIN_ALIGNMENT != 0 || inverse < R3D_SKIN_HEADER_SIZE
        || records < inverse_end || (uint64_t)records + (uint64_t)vertices * stride != entry.size) {
        return ASSET_ERR_BOUNDS;
    }
    if (!asset_entry_zero(entry, R3D_SKIN_HEADER_SIZE, inverse)
        || !asset_entry_zero(entry, (uint32_t)inverse_end, records)) {
        return ASSET_ERR_FORMAT;
    }
    *out = (r3d_skin_t){
        .joint_count = joints,
        .influences = influences,
        .vertex_count = vertices,
        .inverse_binds = (const float(*)[R3D_SKIN_MATRIX_WIDTH])(const void*)(entry.data + inverse),
        .vertices = entry.data + records,
        .vertex_stride = stride,
    };
    return ASSET_OK;
}

static asset_status_t
check_inverse_binds(const r3d_skin_t* skin) {
    for (int i = 0; i < skin->joint_count; i++) {
        for (int k = 0; k < R3D_SKIN_MATRIX_WIDTH; k++) {
            if (!isfinite(skin->inverse_binds[i][k])) {
                return ASSET_ERR_FORMAT;
            }
        }
    }
    return ASSET_OK;
}

static asset_status_t
check_record(const r3d_skin_t* skin, const uint8_t* row) {
    unsigned sum = 0;
    for (int k = 0; k < skin->influences; k++) {
        if (row[k] >= skin->joint_count) {
            return ASSET_ERR_FORMAT;
        }
        sum += row[skin->influences + k];
    }
    if (sum != R3D_SKIN_WEIGHT_SUM || row[skin->vertex_stride - 1] != 0) {
        return ASSET_ERR_FORMAT;
    }
    int nonzero = 0;
    for (int k = 0; k < R3D_SKIN_NORMAL_WIDTH; k++) {
        const int8_t value = (int8_t)row[skin->influences * 2 + k];
        if (value < -R3D_SKIN_NORMAL_SCALE) {
            return ASSET_ERR_FORMAT;
        }
        nonzero |= value;
    }
    if (nonzero == 0) {
        return ASSET_ERR_FORMAT;
    }
    return ASSET_OK;
}

static asset_status_t
check_records(const r3d_skin_t* skin) {
    for (uint32_t i = 0; i < skin->vertex_count; i++) {
        const uint8_t* row = skin->vertices + (size_t)i * skin->vertex_stride;
        const asset_status_t status = check_record(skin, row);
        if (status != ASSET_OK) {
            return status;
        }
    }
    return ASSET_OK;
}

/* The header, the inverse binds, then every vertex record, of an opened entry. */
static asset_status_t
parse(asset_view_t entry, r3d_skin_t* skin) {
    asset_status_t status = read_header(entry, skin);
    status = status == ASSET_OK ? check_inverse_binds(skin) : status;
    return status == ASSET_OK ? check_records(skin) : status;
}

asset_status_t
r3d_skin_open(const asset_pack_t* pack, const char* id, r3d_skin_t* out) {
    *out = (r3d_skin_t){0};
    asset_view_t entry;
    r3d_skin_t skin;
    asset_status_t status = asset_entry_open(pack, id, R3D_SKIN_ASSET, R3D_SKIN_VERSION, R3D_SKIN_AT_VERSION,
                                             R3D_SKIN_HEADER_SIZE, R3D_SKIN_ALIGNMENT, &entry);
    status = status == ASSET_OK ? parse(entry, &skin) : status;
    if (status == ASSET_OK) {
        *out = skin;
    }
    return status;
}
