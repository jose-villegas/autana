/*
 * asset_entry: fixed entry headers and zero gaps shared by mapped readers.
 * The caller owns the format version, header size and alignment.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "asset/asset_pack.h"

#define ASSET_ENTRY_UNVERSIONED 0U

/* Version zero identifies a format without a version field. */
static inline asset_status_t
asset_entry_validate(asset_view_t entry, uint16_t version, uint32_t version_offset, uint32_t header_size,
                     uint32_t alignment) {
    if (entry.data == NULL || entry.size < header_size || (uintptr_t)entry.data % alignment != 0) {
        return ASSET_ERR_BOUNDS;
    }
    if (version != ASSET_ENTRY_UNVERSIONED) {
        uint16_t stored_version;
        memcpy(&stored_version, entry.data + version_offset, sizeof stored_version);
        if (stored_version != version) {
            return ASSET_ERR_VERSION;
        }
    }
    return ASSET_OK;
}

static inline asset_status_t
asset_entry_open(const asset_pack_t* pack, const char* id, uint32_t type, uint16_t version, uint32_t version_offset,
                 uint32_t header_size, uint32_t alignment, asset_view_t* out) {
    *out = (asset_view_t){0};
    asset_view_t entry;
    asset_status_t status = asset_pack_find(pack, id, type, &entry);
    if (status == ASSET_OK) {
        status = asset_entry_validate(entry, version, version_offset, header_size, alignment);
    }
    if (status == ASSET_OK) {
        *out = entry;
    }
    return status;
}

/* The caller has checked first <= end <= entry.size. */
static inline bool
asset_entry_zero(asset_view_t entry, uint32_t first, uint32_t end) {
    for (uint32_t i = first; i < end; i++) {
        if (entry.data[i] != 0) {
            return false;
        }
    }
    return true;
}
