#include "asset/asset_pack.h"

#include <stdint.h>
#include <string.h>

/* The header's byte offsets within a pack, and an entry's within its row. */
enum {
    HEADER_VERSION = 4,
    HEADER_COUNT = 8,
    HEADER_TOTAL = 12,
    HEADER_CRC = 16,
    HEADER_RESERVED = 20,
    ENTRY_NAME = 0,
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
    static const uint32_t nibble[16] = {0x00000000U, 0x1DB71064U, 0x3B6E20C8U, 0x26D930ACU, 0x76DC4190U, 0x6B6B51F4U,
                                        0x4DB26158U, 0x5005713CU, 0xEDB88320U, 0xF00F9344U, 0xD6D6A3E8U, 0xCB61B38CU,
                                        0x9B64C2B0U, 0x86D3D2D4U, 0xA00AE278U, 0xBDBDF21CU};
    uint32_t crc = 0xFFFFFFFFU;
    for (size_t i = 0; i < size; i++) {
        crc ^= bytes[i];
        crc = (crc >> 4) ^ nibble[crc & 15U];
        crc = (crc >> 4) ^ nibble[crc & 15U];
    }
    return ~crc;
}

uint32_t
asset_pack_total_size(const void* head, size_t available) {
    const uint8_t* bytes = head;
    if (bytes == NULL || available < ASSET_PACK_HEADER_SIZE || memcmp(bytes, ASSET_PACK_MAGIC, 4) != 0) {
        return 0;
    }
    return read_u32(bytes + HEADER_TOTAL);
}

asset_status_t
asset_pack_entry(const asset_pack_t* pack, uint32_t index, asset_entry_t* entry) {
    *entry = (asset_entry_t){0};
    if (pack == NULL || pack->base == NULL || index >= pack->count) {
        return ASSET_ERR_NOT_FOUND;
    }
    const uint8_t* row = pack->base + ASSET_PACK_HEADER_SIZE + ((size_t)index * ASSET_PACK_ENTRY_SIZE);
    const uint32_t offset = read_u32(row + ENTRY_OFFSET);
    const uint32_t size = read_u32(row + ENTRY_SIZE);
    const uint32_t align = read_u32(row + ENTRY_ALIGN);
    const uint32_t table_end = ASSET_PACK_HEADER_SIZE + (pack->count * ASSET_PACK_ENTRY_SIZE);
    if (align == 0 || (align & (align - 1U)) != 0 || offset % align != 0 || offset < table_end) {
        return ASSET_ERR_BOUNDS;
    }
    /* Subtracting keeps a huge size from wrapping the sum. */
    if (offset > pack->size || size > pack->size - offset) {
        return ASSET_ERR_BOUNDS;
    }
    *entry = (asset_entry_t){.name = (const char*)row + ENTRY_NAME,
                             .type = read_u32(row + ENTRY_TYPE),
                             .view = {.data = pack->base + offset, .size = size}};
    return ASSET_OK;
}

static bool
reserved_is_zero(const uint8_t* header) {
    for (size_t i = HEADER_RESERVED; i < ASSET_PACK_HEADER_SIZE; i++) {
        if (header[i] != 0) {
            return false;
        }
    }
    return true;
}

asset_status_t
asset_pack_open(asset_pack_t* pack, const void* base, size_t size) {
    const uint8_t* bytes = base;
    *pack = (asset_pack_t){0};
    if (bytes == NULL || size == 0) {
        return ASSET_ERR_NO_PACK;
    }
    if ((uintptr_t)base % ASSET_PACK_BASE_ALIGN != 0) {
        return ASSET_ERR_BOUNDS; /* entries are aligned within the pack, so the pack must be too */
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
    if (total < ASSET_PACK_HEADER_SIZE || !reserved_is_zero(bytes)) {
        return ASSET_ERR_SIZE;
    }
    if (count > (total - ASSET_PACK_HEADER_SIZE) / ASSET_PACK_ENTRY_SIZE) {
        return ASSET_ERR_BOUNDS;
    }
    if (asset_crc32(bytes + ASSET_PACK_HEADER_SIZE, total - ASSET_PACK_HEADER_SIZE) != read_u32(bytes + HEADER_CRC)) {
        return ASSET_ERR_CRC;
    }
    const asset_pack_t opened = {.base = bytes, .size = total, .count = count};
    asset_entry_t entry;
    for (uint32_t i = 0; i < count; i++) {
        const asset_status_t status = asset_pack_entry(&opened, i, &entry);
        if (status != ASSET_OK) {
            return status;
        }
    }
    *pack = opened;
    return ASSET_OK;
}

asset_status_t
asset_pack_find(const asset_pack_t* pack, const char* name, uint32_t type, asset_view_t* view) {
    *view = (asset_view_t){0};
    if (pack == NULL || pack->base == NULL) {
        return ASSET_ERR_NO_PACK;
    }
    asset_entry_t entry;
    for (uint32_t i = 0; i < pack->count; i++) {
        if (asset_pack_entry(pack, i, &entry) != ASSET_OK || strncmp(entry.name, name, ASSET_NAME_MAX) != 0) {
            continue;
        }
        if (entry.type != type) {
            return ASSET_ERR_TYPE;
        }
        *view = entry.view;
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
        case ASSET_ERR_SIZE: return "the pack's header is malformed";
        case ASSET_ERR_CRC: return "the pack's checksum does not match";
        case ASSET_ERR_BOUNDS: return "an entry leaves the pack or is misaligned";
        case ASSET_ERR_NOT_FOUND: return "no such asset";
        case ASSET_ERR_TYPE: return "the asset is not that type";
    }
    return "unknown";
}
