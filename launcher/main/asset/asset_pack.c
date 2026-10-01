#include "asset/asset_pack.h"

#include <string.h>

/* The table's byte offsets within an entry, and the header's within a pack. */
enum {
    HEADER_VERSION = 4,
    HEADER_COUNT = 8,
    HEADER_TOTAL = 12,
    HEADER_CRC = 16,
    ENTRY_TYPE = 32,
    ENTRY_OFFSET = 36,
    ENTRY_SIZE = 40,
    ENTRY_ALIGN = 44,
};

static uint32_t
read_u32(const uint8_t* at) {
    return (uint32_t)at[0] | ((uint32_t)at[1] << 8) | ((uint32_t)at[2] << 16) | ((uint32_t)at[3] << 24);
}

/* CRC-32 as zlib computes it, a nibble at a time: no 1 KiB table in RAM, and
 * the pack is read once. */
uint32_t
asset_crc32(const void* data, size_t size) {
    const uint8_t* bytes = data;
    static const uint32_t nibble[16] = {0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu, 0x76DC4190u, 0x6B6B51F4u,
                                        0x4DB26158u, 0x5005713Cu, 0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
                                        0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu};
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; i++) {
        crc ^= bytes[i];
        crc = (crc >> 4) ^ nibble[crc & 15u];
        crc = (crc >> 4) ^ nibble[crc & 15u];
    }
    return ~crc;
}

static asset_status_t
check_entry(const uint8_t* entry, uint32_t total) {
    const uint32_t offset = read_u32(entry + ENTRY_OFFSET);
    const uint32_t size = read_u32(entry + ENTRY_SIZE);
    const uint32_t align = read_u32(entry + ENTRY_ALIGN);
    if (align == 0 || (align & (align - 1u)) != 0 || offset % align != 0) {
        return ASSET_ERR_BOUNDS;
    }
    /* Subtracting keeps a huge size from wrapping the sum. */
    if (offset > total || size > total - offset) {
        return ASSET_ERR_BOUNDS;
    }
    return ASSET_OK;
}

asset_status_t
asset_pack_open(asset_pack_t* pack, const void* base, size_t size) {
    const uint8_t* bytes = base;
    *pack = (asset_pack_t){0};
    if (bytes == NULL || size == 0) {
        return ASSET_ERR_NO_PACK;
    }
    if (size < ASSET_PACK_HEADER_SIZE) {
        return ASSET_ERR_TRUNCATED;
    }
    if (memcmp(bytes, ASSET_PACK_MAGIC, 4) != 0) {
        return ASSET_ERR_MAGIC;
    }
    if (read_u32(bytes + HEADER_VERSION) != ASSET_PACK_VERSION) {
        return ASSET_ERR_VERSION;
    }
    const uint32_t count = read_u32(bytes + HEADER_COUNT);
    const uint32_t total = read_u32(bytes + HEADER_TOTAL);
    if (total > size) {
        return ASSET_ERR_TRUNCATED; /* a partition may be larger than the pack it holds, never smaller */
    }
    if (total < ASSET_PACK_HEADER_SIZE) {
        return ASSET_ERR_SIZE;
    }
    if (count > (total - ASSET_PACK_HEADER_SIZE) / ASSET_PACK_ENTRY_SIZE) {
        return ASSET_ERR_BOUNDS;
    }
    if (asset_crc32(bytes + ASSET_PACK_HEADER_SIZE, total - ASSET_PACK_HEADER_SIZE) != read_u32(bytes + HEADER_CRC)) {
        return ASSET_ERR_CRC;
    }
    const uint32_t table_end = ASSET_PACK_HEADER_SIZE + (count * ASSET_PACK_ENTRY_SIZE);
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t* entry = bytes + ASSET_PACK_HEADER_SIZE + (i * ASSET_PACK_ENTRY_SIZE);
        const asset_status_t status = check_entry(entry, total);
        if (status != ASSET_OK) {
            return status;
        }
        if (read_u32(entry + ENTRY_OFFSET) < table_end) {
            return ASSET_ERR_BOUNDS;
        }
    }
    *pack = (asset_pack_t){.base = bytes, .size = total, .count = count};
    return ASSET_OK;
}

asset_status_t
asset_pack_find(const asset_pack_t* pack, const char* name, uint32_t type, asset_view_t* view) {
    *view = (asset_view_t){0};
    if (pack == NULL || pack->base == NULL) {
        return ASSET_ERR_NO_PACK;
    }
    for (uint32_t i = 0; i < pack->count; i++) {
        const uint8_t* entry = pack->base + ASSET_PACK_HEADER_SIZE + (i * ASSET_PACK_ENTRY_SIZE);
        if (strncmp((const char*)entry, name, ASSET_NAME_MAX) != 0) {
            continue;
        }
        if (read_u32(entry + ENTRY_TYPE) != type) {
            return ASSET_ERR_TYPE;
        }
        *view =
            (asset_view_t){.data = pack->base + read_u32(entry + ENTRY_OFFSET), .size = read_u32(entry + ENTRY_SIZE)};
        return ASSET_OK;
    }
    return ASSET_ERR_NOT_FOUND;
}

const char*
asset_status_text(asset_status_t status) {
    switch (status) {
        case ASSET_OK: return "ok";
        case ASSET_ERR_NO_PACK: return "no asset pack is mapped";
        case ASSET_ERR_TRUNCATED: return "the pack is shorter than its header says";
        case ASSET_ERR_MAGIC: return "not an asset pack";
        case ASSET_ERR_VERSION: return "an asset pack format this firmware does not read";
        case ASSET_ERR_SIZE: return "the pack's size is not its header's";
        case ASSET_ERR_CRC: return "the pack's checksum does not match";
        case ASSET_ERR_BOUNDS: return "an entry leaves the pack or is misaligned";
        case ASSET_ERR_NOT_FOUND: return "no such asset";
        case ASSET_ERR_TYPE: return "the asset is not that type";
    }
    return "unknown";
}
