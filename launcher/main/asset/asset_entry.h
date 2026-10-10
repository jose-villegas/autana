/*
 * asset_entry: fixed entry headers and zero gaps shared by mapped readers.
 * The caller owns the format version, header size and alignment.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "asset/asset_pack.h"

/* Callers supply the format's header size and alignment; version is its leading u16. */
static inline asset_status_t
asset_entry_open(const asset_pack_t* pack, const char* id, uint32_t type, uint16_t version, uint32_t header_size,
                 uint32_t alignment, asset_view_t* out) {
    *out = (asset_view_t){0};
    asset_view_t entry;
    asset_status_t status = asset_pack_find(pack, id, type, &entry);
    if (status != ASSET_OK) {
        return status;
    }
    if (entry.size < header_size || (uintptr_t)entry.data % alignment != 0) {
        return ASSET_ERR_BOUNDS;
    }
    uint16_t stored_version;
    memcpy(&stored_version, entry.data, sizeof stored_version);
    if (stored_version != version) {
        return ASSET_ERR_VERSION;
    }
    *out = entry;
    return ASSET_OK;
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
