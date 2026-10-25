/* load_pack: request on enter, read each frame, release on exit. */
#include "asset/asset_store.h"

typedef struct {
    const char* name;
    handle_t load;
} load_pack_example_t;

void
load_pack_enter(load_pack_example_t* visit, const char* name) {
    *visit = (load_pack_example_t){.name = name, .load = asset_store_load_async(name, NULL, HANDLE_NONE)};
}

const asset_pack_t*
load_pack_frame(const load_pack_example_t* visit) {
    return asset_store_loaded(visit->load);
}

void
load_pack_exit(load_pack_example_t* visit) {
    if (asset_store_state(visit->load) == PENDING_READY) {
        asset_store_release(visit->name);
    } else {
        asset_store_cancel(visit->load);
    }
    visit->load = HANDLE_NONE;
}

asset_status_t
load_pack_example(const char* pack_name, const char* entry, uint32_t type, uint32_t* size) {
    const asset_pack_t* pack = asset_store_load(pack_name);
    if (pack == NULL) {
        return ASSET_ERR_NO_PACK; /* the log line says why: missing, bad CRC, store full */
    }
    asset_view_t view;
    const asset_status_t status = asset_pack_find(pack, entry, type, &view);
    if (status == ASSET_OK) {
        *size = view.size; /* view.data points into the pack: use it before the release */
    }
    asset_store_release(pack_name);
    return status;
}
