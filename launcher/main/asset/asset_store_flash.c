#include "asset/asset_store.h"

#include <stdbool.h>

#include "esp_log.h"
#include "esp_partition.h"

/* The first of the data subtypes ESP-IDF leaves for an application. */
#define ASSET_PARTITION_SUBTYPE 0x40
#define ASSET_PARTITION_LABEL   "assets"

static const char* TAG = "asset";

static asset_pack_t pack;
static asset_status_t status = ASSET_ERR_NO_PACK;
static bool tried;

/* Maps the first `bytes` of the partition. */
static const void*
map_bytes(const esp_partition_t* part, size_t bytes, esp_partition_mmap_handle_t* handle) {
    const void* mapped = NULL;
    const esp_err_t err = esp_partition_mmap(part, 0, bytes, ESP_PARTITION_MMAP_DATA, &mapped, handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mapping the '%s' partition failed: %s", ASSET_PARTITION_LABEL, esp_err_to_name(err));
        return NULL;
    }
    return mapped;
}

/* The pack's size is in its header, so only the pack is mapped, not the
 * partition around it. */
static asset_status_t
map_partition(void) {
    const esp_partition_t* part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)ASSET_PARTITION_SUBTYPE, ASSET_PARTITION_LABEL);
    if (part == NULL) {
        ESP_LOGE(TAG, "no '%s' partition: the flashed partition table is older than this firmware, flash it",
                 ASSET_PARTITION_LABEL);
        return ASSET_ERR_NO_PACK;
    }
    esp_partition_mmap_handle_t handle;
    const void* head = map_bytes(part, ASSET_PACK_HEADER_SIZE, &handle);
    if (head == NULL) {
        return ASSET_ERR_NO_PACK;
    }
    const uint32_t total = asset_pack_total_size(head, ASSET_PACK_HEADER_SIZE);
    esp_partition_munmap(handle);
    if (total == 0 || total > part->size) {
        ESP_LOGE(TAG, "the '%s' partition holds no asset pack: flash it", ASSET_PARTITION_LABEL);
        return total == 0 ? ASSET_ERR_MAGIC : ASSET_ERR_TRUNCATED;
    }
    const void* mapped = map_bytes(part, total, &handle);
    if (mapped == NULL) {
        return ASSET_ERR_NO_PACK;
    }
    const asset_status_t opened = asset_pack_open(&pack, mapped, total);
    if (opened != ASSET_OK) {
        ESP_LOGE(TAG, "the asset pack is unusable (%s): flash it", asset_status_text(opened));
        esp_partition_munmap(handle);
    }
    return opened;
}

const asset_pack_t*
asset_store_pack(void) {
    if (!tried) {
        tried = true;
        status = map_partition();
        if (status == ASSET_OK) {
            ESP_LOGI(TAG, "asset pack: %u entries, %u bytes", (unsigned)pack.count, (unsigned)pack.size);
        }
    }
    return status == ASSET_OK ? &pack : NULL;
}
