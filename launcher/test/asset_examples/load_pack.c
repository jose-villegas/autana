/* load_pack: read one entry while holding a counted pack use. */
#include "asset/asset_store.h"

/* Blocking form: small packs, startup and tests. */
asset_status_t
load_pack_example(const char* pack_name, const char* entry, uint32_t type, uint32_t* size) {
    const handle_t load = asset_store_load(pack_name);
    const asset_pack_t* pack = asset_store_loaded(load);
    if (pack == NULL) {
        asset_store_release(load);
        return ASSET_ERR_NO_PACK; /* the log line says why: missing, bad CRC, store full */
    }
    asset_view_t view;
    const asset_status_t status = asset_pack_find(pack, entry, type, &view);
    if (status == ASSET_OK) {
        *size = view.size; /* view.data points into the pack: use it before the release */
    }
    asset_store_release(load);
    return status;
}
