#include "asset/asset_table.h"

#include <string.h>

#include "asset/asset_bytes.h"

asset_status_t
asset_table_open(asset_view_t entry, asset_table_t* out) {
    *out = (asset_table_t){0};
    if (entry.size < ASSET_TABLE_HEADER_SIZE || (uintptr_t)entry.data % sizeof(int16_t) != 0) {
        return ASSET_ERR_BOUNDS;
    }
    const uint8_t* base = entry.data;
    if (asset_read_u16(base) != ASSET_TABLE_VERSION) {
        return ASSET_ERR_VERSION;
    }
    const uint16_t rows = asset_read_u16(base + ASSET_TABLE_AT_ROWS);
    const uint16_t columns = asset_read_u16(base + ASSET_TABLE_AT_COLUMNS);
    const size_t stride = ASSET_TABLE_NAME_SIZE + (sizeof(int16_t) * columns);
    const uint64_t size = ASSET_TABLE_HEADER_SIZE + ((uint64_t)rows * stride);
    if (rows == 0 || columns == 0 || base[ASSET_TABLE_AT_RESERVED] != 0 || size != entry.size) {
        return ASSET_ERR_FORMAT;
    }
    for (int i = 0; i < rows; i++) {
        if (memchr(base + ASSET_TABLE_HEADER_SIZE + ((size_t)i * stride), 0, ASSET_TABLE_NAME_SIZE) == NULL) {
            return ASSET_ERR_FORMAT;
        }
    }
    *out = (asset_table_t){base, rows, columns, base[ASSET_TABLE_AT_FRAC_BITS]};
    return ASSET_OK;
}

asset_status_t
asset_table_row(const asset_table_t* table, const char* name, int columns, const int16_t** out) {
    *out = NULL;
    if (columns != table->columns) {
        return ASSET_ERR_FORMAT;
    }
    const size_t stride = ASSET_TABLE_NAME_SIZE + (sizeof(int16_t) * table->columns);
    for (int i = 0; i < table->rows; i++) {
        const uint8_t* row = table->base + ASSET_TABLE_HEADER_SIZE + ((size_t)i * stride);
        if (strcmp((const char*)row, name) == 0) {
            *out = (const int16_t*)(const void*)(row + ASSET_TABLE_NAME_SIZE);
            return ASSET_OK;
        }
    }
    return ASSET_ERR_NOT_FOUND;
}
