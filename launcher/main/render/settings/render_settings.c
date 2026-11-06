#include "render/settings/render_settings.h"

#include "asset/asset_table.h"
#include "asset/asset_table_format.h"

#define COUNT_FIELD(field) +1

asset_status_t
render_settings_read(const asset_pack_t* pack, render_settings_t* out) {
    asset_view_t entry;
    asset_table_t table;
    asset_status_t status = asset_pack_find(pack, RENDER_SETTINGS_ENTRY, ASSET_TABLE_ASSET, &entry);
    if (status == ASSET_OK) {
        status = asset_table_open(entry, &table);
    }
    if (status == ASSET_OK && (table.rows != (0 RENDER_SETTINGS_FIELDS(COUNT_FIELD)) || table.frac_bits != 0)) {
        status = ASSET_ERR_FORMAT;
    }
    render_settings_t read = {0};
    const int16_t* value = NULL;
#define READ_FIELD(field)                                                                                              \
    if (status == ASSET_OK) {                                                                                          \
        status = asset_table_row(&table, #field, 1, &value) == ASSET_OK ? ASSET_OK : ASSET_ERR_FORMAT;                 \
    }                                                                                                                  \
    if (status == ASSET_OK) {                                                                                          \
        read.field = *value;                                                                                           \
    }
    RENDER_SETTINGS_FIELDS(READ_FIELD)
    if (status == ASSET_OK) {
        *out = read;
    }
    return status;
}
