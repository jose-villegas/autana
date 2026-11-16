/* render_system: the main render context's settings from the starting app's pack. */
#include "render/context/render_context.h"
#include "render/settings/render_settings.h"
#include "shell/shell_system.h"

#include "esp_log.h"

static const char* TAG = "render_system";

/* The pack's render entry, or RENDER_SETTINGS_DEFAULT without one, so no app
 * starts with the last app's settings. */
static void
render_app_enter(const asset_pack_t* pack) {
    render_settings_t settings = RENDER_SETTINGS_DEFAULT;
    const asset_status_t status = pack != NULL ? render_settings_read(pack, &settings) : ASSET_ERR_NOT_FOUND;
    if (status != ASSET_OK && status != ASSET_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "entry %s: %s: default render settings", RENDER_SETTINGS_ENTRY, asset_status_text(status));
    }
    render_context_set_settings(render_context_main(), &settings);
}

static shell_system_t render_system = {
    .name = "render",
    .order = SHELL_ORDER_RENDER,
    .app_enter = render_app_enter,
};
SHELL_SYSTEM_REGISTER(render_system);
