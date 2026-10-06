#include "asset/asset_directory.h"

#include <string.h>

#include "asset/asset_bytes.h"

/* The header's byte offsets, and a row's within itself. */
enum {
    HEADER_CRC = 4,
    HEADER_VERSION = 8,
    HEADER_COUNT = 12,
    ROW_OFFSET = 32,
    ROW_SIZE = 36,
};

static const uint8_t*
row_at(const uint8_t* base, uint32_t index) {
    return base + ASSET_DIRECTORY_HEADER_SIZE + ((size_t)index * ASSET_DIRECTORY_ROW_SIZE);
}

/* A name is NUL padded and leaves room for the NUL. */
static bool
name_is_valid(const uint8_t* row) {
    return row[0] != 0 && memchr(row, 0, ASSET_NAME_MAX) != NULL;
}

uint32_t
asset_directory_size(const void* head, size_t available) {
    const uint8_t* bytes = head;
    if (bytes == NULL || available < ASSET_DIRECTORY_HEADER_SIZE || memcmp(bytes, ASSET_DIRECTORY_MAGIC, 4) != 0) {
        return 0;
    }
    const uint32_t count = asset_read_u32(bytes + HEADER_COUNT);
    if (count > (UINT32_MAX - ASSET_DIRECTORY_HEADER_SIZE) / ASSET_DIRECTORY_ROW_SIZE) {
        return 0;
    }
    return ASSET_DIRECTORY_HEADER_SIZE + (count * ASSET_DIRECTORY_ROW_SIZE);
}

/* Each row in turn: a valid name, on a sector, after the rows and the pack
 * before it, inside the region, its name not seen before. */
static asset_status_t
check_rows(const uint8_t* bytes, uint32_t count, uint32_t region) {
    uint32_t end = ASSET_DIRECTORY_HEADER_SIZE + (count * ASSET_DIRECTORY_ROW_SIZE);
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t* row = row_at(bytes, i);
        const uint32_t offset = asset_read_u32(row + ROW_OFFSET);
        const uint32_t size = asset_read_u32(row + ROW_SIZE);
        if (!name_is_valid(row) || offset % ASSET_PACK_ALIGN != 0 || offset < end) {
            return ASSET_ERR_BOUNDS;
        }
        if (offset > region || size > region - offset) {
            return ASSET_ERR_BOUNDS;
        }
        for (uint32_t j = 0; j < i; j++) {
            if (strncmp((const char*)row, (const char*)row_at(bytes, j), ASSET_NAME_MAX) == 0) {
                return ASSET_ERR_DUPLICATE;
            }
        }
        end = offset + size;
    }
    return ASSET_OK;
}

asset_status_t
asset_directory_open(asset_directory_t* directory, const void* base, size_t size, uint32_t region) {
    const uint8_t* bytes = base;
    *directory = (asset_directory_t){0};
    if (bytes == NULL || size == 0) {
        return ASSET_ERR_NO_PACK;
    }
    if (size < ASSET_DIRECTORY_HEADER_SIZE) {
        return ASSET_ERR_TRUNCATED;
    }
    if (memcmp(bytes, ASSET_DIRECTORY_MAGIC, 4) != 0) {
        return ASSET_ERR_MAGIC;
    }
    if (asset_read_u32(bytes + HEADER_VERSION) != ASSET_DIRECTORY_VERSION) {
        return ASSET_ERR_VERSION;
    }
    const uint32_t end = asset_directory_size(bytes, size);
    if (end == 0 || end > size || end > region) {
        return ASSET_ERR_TRUNCATED;
    }
    if (asset_crc32(bytes + HEADER_VERSION, end - HEADER_VERSION) != asset_read_u32(bytes + HEADER_CRC)) {
        return ASSET_ERR_CRC;
    }
    const uint32_t count = asset_read_u32(bytes + HEADER_COUNT);
    const asset_status_t rows = check_rows(bytes, count, region);
    if (rows == ASSET_OK) {
        *directory = (asset_directory_t){.base = bytes, .count = count};
    }
    return rows;
}

asset_status_t
asset_directory_find(const asset_directory_t* directory, const char* name, asset_slot_t* slot) {
    *slot = (asset_slot_t){0};
    for (uint32_t i = 0; directory->base != NULL && i < directory->count; i++) {
        const uint8_t* row = row_at(directory->base, i);
        if (strncmp((const char*)row, name, ASSET_NAME_MAX) == 0) {
            *slot = (asset_slot_t){.offset = asset_read_u32(row + ROW_OFFSET), .size = asset_read_u32(row + ROW_SIZE)};
            return ASSET_OK;
        }
    }
    return ASSET_ERR_NOT_FOUND;
}
