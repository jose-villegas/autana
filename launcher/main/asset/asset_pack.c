#include "asset/asset_pack.h"

#include <stdint.h>
#include <string.h>

#include "asset/asset_bytes.h"
#include "hash/crc32.h"

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

uint32_t
asset_pack_total_size(const void* head, size_t available) {
    const uint8_t* bytes = head;
    if (bytes == NULL || available < ASSET_PACK_HEADER_SIZE || memcmp(bytes, ASSET_PACK_MAGIC, 4) != 0) {
        return 0;
    }
    return asset_read_u32(bytes + HEADER_TOTAL);
}

asset_status_t
asset_pack_entry(const asset_pack_t* pack, uint32_t index, asset_entry_t* entry) {
    *entry = (asset_entry_t){0};
    if (pack == NULL || pack->base == NULL || index >= pack->count) {
        return ASSET_ERR_NOT_FOUND;
    }
    const uint8_t* row = pack->base + ASSET_PACK_HEADER_SIZE + ((size_t)index * ASSET_PACK_ENTRY_SIZE);
    const uint32_t offset = asset_read_u32(row + ENTRY_OFFSET);
    const uint32_t size = asset_read_u32(row + ENTRY_SIZE);
    const uint32_t align = asset_read_u32(row + ENTRY_ALIGN);
    const uint32_t table_end = ASSET_PACK_HEADER_SIZE + (pack->count * ASSET_PACK_ENTRY_SIZE);
    if (align == 0 || (align & (align - 1U)) != 0 || offset % align != 0 || offset < table_end) {
        return ASSET_ERR_BOUNDS;
    }
    /* Subtracting keeps a huge size from wrapping the sum. */
    if (offset > pack->size || size > pack->size - offset) {
        return ASSET_ERR_BOUNDS;
    }
    *entry = (asset_entry_t){.name = (const char*)row + ENTRY_NAME,
                             .type = asset_read_u32(row + ENTRY_TYPE),
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
asset_pack_open_begin(asset_pack_opener_t* opener, const void* base, size_t size) {
    const uint8_t* bytes = base;
    *opener = (asset_pack_opener_t){0};
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
    if (asset_read_u32(bytes + HEADER_VERSION) != ASSET_PACK_VERSION) {
        return ASSET_ERR_VERSION;
    }
    const uint32_t count = asset_read_u32(bytes + HEADER_COUNT);
    const uint32_t total = asset_read_u32(bytes + HEADER_TOTAL);
    if (total > size) {
        return ASSET_ERR_TRUNCATED; /* a partition may be larger than the pack it holds, never smaller */
    }
    if (total < ASSET_PACK_HEADER_SIZE || !reserved_is_zero(bytes)) {
        return ASSET_ERR_SIZE;
    }
    if (count > (total - ASSET_PACK_HEADER_SIZE) / ASSET_PACK_ENTRY_SIZE) {
        return ASSET_ERR_BOUNDS;
    }
    opener->pack = (asset_pack_t){.base = bytes, .size = total, .count = count};
    opener->offset = ASSET_PACK_HEADER_SIZE;
    return ASSET_OK;
}

bool
asset_pack_open_step(asset_pack_opener_t* opener, size_t budget) {
    if (opener->complete) {
        return true;
    }
    const asset_pack_t* pack = &opener->pack;
    if (opener->offset < pack->size) {
        size_t bytes = pack->size - opener->offset;
        if (bytes > budget) {
            bytes = budget;
        }
        opener->crc = crc32_update(opener->crc, pack->base + opener->offset, bytes);
        opener->offset += (uint32_t)bytes;
        budget -= bytes;
        if (opener->offset < pack->size) {
            return false;
        }
    }
    if (opener->crc != asset_read_u32(pack->base + HEADER_CRC)) {
        opener->status = ASSET_ERR_CRC;
        opener->complete = true;
        return true;
    }
    while (opener->entry < pack->count && budget >= ASSET_PACK_ENTRY_SIZE) {
        asset_entry_t entry;
        opener->status = asset_pack_entry(pack, opener->entry++, &entry);
        budget -= ASSET_PACK_ENTRY_SIZE;
        if (opener->status != ASSET_OK) {
            break;
        }
    }
    opener->complete = opener->status != ASSET_OK || opener->entry == pack->count;
    return opener->complete;
}

asset_status_t
asset_pack_open(asset_pack_t* pack, const void* base, size_t size) {
    *pack = (asset_pack_t){0};
    asset_pack_opener_t opener;
    asset_status_t status = asset_pack_open_begin(&opener, base, size);
    if (status != ASSET_OK) {
        return status;
    }
    while (!asset_pack_open_step(&opener, SIZE_MAX)) {}
    if (opener.status == ASSET_OK) {
        *pack = opener.pack;
    }
    return opener.status;
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
        case ASSET_ERR_NO_PACK: return "no such pack is mapped or read";
        case ASSET_ERR_TRUNCATED: return "the pack is shorter than its header says";
        case ASSET_ERR_MAGIC: return "not an asset pack";
        case ASSET_ERR_VERSION: return "a pack or entry format this firmware does not read";
        case ASSET_ERR_SIZE: return "the pack's header is malformed";
        case ASSET_ERR_CRC: return "the pack's checksum does not match";
        case ASSET_ERR_BOUNDS: return "an entry leaves the pack or is misaligned";
        case ASSET_ERR_NOT_FOUND: return "no such asset";
        case ASSET_ERR_TYPE: return "the asset is not that type";
        case ASSET_ERR_FORMAT: return "the asset holds a value its reader does not accept";
        case ASSET_ERR_DUPLICATE: return "two packs share a name";
        case ASSET_ERR_FULL: return "as many packs are mounted as the store holds";
    }
    return "unknown";
}
