/* asset_table: named int16 rows read in place from a pack entry. */
#pragma once

#include "asset/asset_pack.h"

typedef struct {
    const uint8_t* base;
    uint16_t rows, columns;
    uint8_t frac_bits;
} asset_table_t;

asset_status_t asset_table_open(asset_view_t entry, asset_table_t* out);
/* A row's values point into the entry, which must outlive the table. */
asset_status_t asset_table_row(const asset_table_t* table, const char* name, int columns, const int16_t** out);
