/* Look a row up in a table entry: the first value of a named row, as a real number. */
#include "asset/asset_table.h"
#include "asset/asset_table_format.h"

asset_status_t
table_example(const asset_pack_t* pack, const char* table_name, const char* row_name, float* first) {
    asset_view_t entry;
    asset_table_t table;
    const int16_t* row = NULL;
    asset_status_t status = asset_pack_find(pack, table_name, ASSET_TABLE_ASSET, &entry);
    if (status == ASSET_OK) {
        status = asset_table_open(entry, &table);
    }
    if (status == ASSET_OK) {
        status = asset_table_row(&table, row_name, table.columns, &row);
    }
    if (status == ASSET_OK) {
        *first = (float)row[0] / (float)(1U << table.frac_bits); /* values are fixed point */
    }
    return status;
}
