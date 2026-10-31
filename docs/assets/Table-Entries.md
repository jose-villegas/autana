# Table entries

Part of the [asset docs](README.md). Named rows of int16 values are one entry of type `TABL`, read in place ([TABL v1](formats/TABL.md)).

[`table_asset.py`](../../launcher/tools/asset/table_asset.py) writes generic
numeric tables. [`curve_asset.py`](../../launcher/tools/gfx/curve_asset.py)
bakes a `NAME.curve.toml` and the PNG it names into one: one bright line
crossing every column of the panel's landscape frame, stored as row `y` of
brightness-weighted heights.

## Example

Find a table entry, look a row up by name and read its first value as a real number.

<!-- generated: asset-table-example check: python launcher/tools/gen/examples_doc.py --check -->
```c
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
```
<!-- /generated: asset-table-example -->
