/*
 * asset_store_flash: the asset store's board backend, mapping bundles from
 * the "assets" flash partition through its directory.
 */
#include <stdbool.h>

#include "asset/asset_directory.h"
#include "asset/asset_store_backend.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "util/runtime/memory.h"

/* The first of the data subtypes ESP-IDF leaves for an application. */
#define ASSET_PARTITION_SUBTYPE 0x40
#define ASSET_PARTITION_LABEL   "assets"

static const char* TAG = "asset";

/* The partition and its directory, found and read once and kept for good. */
static const esp_partition_t* part;
static asset_directory_t directory;
static asset_status_t directory_status = ASSET_ERR_NO_PACK;
static bool tried;

static const void*
map_bytes(size_t offset, size_t bytes, esp_partition_mmap_handle_t* handle) {
    const void* mapped = NULL;
    const esp_err_t err = esp_partition_mmap(part, offset, bytes, ESP_PARTITION_MMAP_DATA, &mapped, handle);
    return err == ESP_OK ? mapped : NULL;
}

/* The directory is read into RAM rather than mapped: it shares its flash page
 * with the first bundles, and QEMU drops every mapping of a page when any one
 * of them is unmapped, so a mapped directory goes blank there once such a
 * bundle is released. A copy is a few bytes per bundle and holds no MMU page. */
static asset_status_t
open_directory(void) {
    part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)ASSET_PARTITION_SUBTYPE,
                                    ASSET_PARTITION_LABEL);
    if (part == NULL) {
        return ASSET_ERR_NO_PACK;
    }
    uint8_t head[ASSET_DIRECTORY_HEADER_SIZE];
    if (esp_partition_read(part, 0, head, sizeof head) != ESP_OK) {
        return ASSET_ERR_NO_PACK;
    }
    const uint32_t size = asset_directory_size(head, sizeof head);
    if (size == 0 || size > part->size) {
        return size == 0 ? ASSET_ERR_MAGIC : ASSET_ERR_TRUNCATED;
    }
    uint8_t* copy = memory_alloc(size, MEMORY_INTERNAL);
    if (copy == NULL) {
        return ASSET_ERR_NO_PACK;
    }
    asset_status_t opened = ASSET_ERR_NO_PACK;
    if (esp_partition_read(part, 0, copy, size) == ESP_OK) {
        opened = asset_directory_open(&directory, copy, size, (uint32_t)part->size);
    }
    if (opened != ASSET_OK) {
        memory_free(copy);
    }
    return opened;
}

asset_status_t
asset_store_backend_mount(const char* name, asset_pack_t* pack, uintptr_t* mapping) {
    if (!tried) {
        tried = true;
        directory_status = open_directory();
    }
    if (directory_status != ASSET_OK) {
        return directory_status;
    }
    asset_slot_t slot;
    const asset_status_t found = asset_directory_find(&directory, name, &slot);
    if (found != ASSET_OK) {
        return found;
    }
    esp_partition_mmap_handle_t handle;
    const void* mapped = map_bytes(slot.offset, slot.size, &handle);
    if (mapped == NULL) {
        return ASSET_ERR_NO_PACK;
    }
    const asset_status_t opened = asset_pack_open(pack, mapped, slot.size);
    if (opened != ASSET_OK) {
        esp_partition_munmap(handle);
        return opened;
    }
    ESP_LOGI(TAG, "bundle %s: %u entries, %u bytes", name, (unsigned)pack->count, (unsigned)pack->size);
    *mapping = (uintptr_t)handle;
    return ASSET_OK;
}

void
asset_store_backend_unmount(uintptr_t mapping) {
    esp_partition_munmap((esp_partition_mmap_handle_t)mapping);
}

void
asset_store_backend_report(const char* name, asset_status_t status) {
    if (part == NULL) {
        ESP_LOGE(TAG, "bundle %s: no '%s' partition: the flashed partition table is older than this firmware, flash it",
                 name, ASSET_PARTITION_LABEL);
    } else if (directory_status != ASSET_OK) {
        ESP_LOGE(TAG, "bundle %s: the '%s' partition holds no usable bundle directory (%s): flash it", name,
                 ASSET_PARTITION_LABEL, asset_status_text(status));
    } else {
        ESP_LOGE(TAG, "bundle %s: %s", name, asset_status_text(status));
    }
}
