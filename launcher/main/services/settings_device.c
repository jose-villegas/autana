/* settings_device: settings.h over the non-volatile storage library. */

#include "services/settings.h"

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "services/settings_policy.h"

static const char TAG[] = "settings";

static bool
store_ready(void) {
    static const settings_store_ops_t ops = {
        .init = nvs_flash_init,
        .erase = nvs_flash_erase,
        .stale_full = ESP_ERR_NVS_NO_FREE_PAGES,
        .stale_format = ESP_ERR_NVS_NEW_VERSION_FOUND,
    };
    const esp_err_t err = settings_store_start(&ops);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS unavailable, settings are not kept: %s", esp_err_to_name(err));
    }
    return err == ESP_OK;
}

bool
settings_read_i32(const char* space, const char* key, int32_t* out) {
    nvs_handle_t handle;
    if (!store_ready() || nvs_open(space, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    const bool found = nvs_get_i32(handle, key, out) == ESP_OK;
    nvs_close(handle);
    return found;
}

bool
settings_write_i32(const char* space, const char* key, int32_t value) {
    nvs_handle_t handle;
    if (!store_ready() || nvs_open(space, NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }
    const bool saved = nvs_set_i32(handle, key, value) == ESP_OK && nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    return saved;
}
