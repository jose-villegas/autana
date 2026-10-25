/* asset_system: asset request progress and completion on each core-0 frame pass. */
#include "asset/asset_store_pending.h"
#include "shell/shell_system.h"

static void
poll(uint32_t dt_ms) {
    (void)dt_ms;
    asset_store_step();
}

static shell_system_t asset_system = {
    .name = "asset",
    .order = SHELL_ORDER_ASSET,
    .poll = poll,
    .app_exit = asset_store_app_exit,
};
SHELL_SYSTEM_REGISTER(asset_system);
