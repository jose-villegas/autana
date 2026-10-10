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

/* One entry format: what asset_entry_open() checks before a reader parses it. */
typedef struct {
    uint32_t type;
    uint16_t version;
    uint32_t version_offset, header_size, alignment;
} asset_entry_format_t;

/* Opens entry `id` as `format` and parses it into `out` (`size` bytes); on
 * any failure `out` is zeroed, so a reader's view is whole or empty. */
static inline asset_status_t
asset_entry_read(const asset_pack_t* pack, const char* id, const asset_entry_format_t* format,
                 asset_status_t (*parse)(asset_view_t entry, void* out), void* out, size_t size) {
    asset_view_t entry;
    asset_status_t status = asset_entry_open(pack, id, format->type, format->version, format->version_offset,
                                             format->header_size, format->alignment, &entry);
    status = status == ASSET_OK ? parse(entry, out) : status;
    if (status != ASSET_OK) {
        memset(out, 0, size);
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
