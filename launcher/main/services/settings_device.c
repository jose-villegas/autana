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

typedef enum { SETTINGS_READ_I32, SETTINGS_READ_I64, SETTINGS_WRITE_I64, SETTINGS_ERASE } settings_operation_t;

static bool
store_open(const char* space, bool reading, nvs_handle_t* handle) {
    return store_ready() && nvs_open(space, reading ? NVS_READONLY : NVS_READWRITE, handle) == ESP_OK;
}

/* Commits a write that succeeded, then closes; true when the operation and commit both held. */
static bool
store_close(nvs_handle_t handle, esp_err_t result, bool reading) {
    const bool ok = result == ESP_OK && (reading || nvs_commit(handle) == ESP_OK);
    nvs_close(handle);
    return ok;
}

static bool
store_access(const char* space, const char* key, void* value, settings_operation_t operation) {
    const bool reading = operation == SETTINGS_READ_I32 || operation == SETTINGS_READ_I64;
    nvs_handle_t handle;
    if (!store_open(space, reading, &handle)) {
        return false;
    }
    esp_err_t result;
    switch (operation) {
        case SETTINGS_READ_I32: result = nvs_get_i32(handle, key, value); break;
        case SETTINGS_READ_I64: result = nvs_get_i64(handle, key, value); break;
        case SETTINGS_WRITE_I64: result = nvs_set_i64(handle, key, *(int64_t*)value); break;
        default: result = nvs_erase_all(handle); break;
    }
    return store_close(handle, result, reading);
}

bool
settings_read_i32(const char* space, const char* key, int32_t* out) {
    return store_access(space, key, out, SETTINGS_READ_I32);
}

bool
settings_write_i32(const char* space, const char* key, int32_t value) {
    return settings_write_i32_batch(space, &key, &value, 1);
}

bool
settings_read_i64(const char* space, const char* key, int64_t* out) {
    return store_access(space, key, out, SETTINGS_READ_I64);
}

bool
settings_write_i64(const char* space, const char* key, int64_t value) {
    return store_access(space, key, &value, SETTINGS_WRITE_I64);
}

bool
settings_erase_space(const char* space) {
    return store_access(space, NULL, NULL, SETTINGS_ERASE);
}

bool
settings_write_i32_batch(const char* space, const char* const keys[], const int32_t values[], size_t n) {
    nvs_handle_t handle;
    if (!store_open(space, false, &handle)) {
        return false;
    }
    esp_err_t result = ESP_OK;
    for (size_t i = 0; i < n && result == ESP_OK; i++) {
        result = nvs_set_i32(handle, keys[i], values[i]);
    }
    return store_close(handle, result, false);
}
