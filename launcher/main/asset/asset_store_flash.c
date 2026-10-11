/*
 * asset_store_flash: the asset store's board backend, mapping packs from
 * the "assets" flash partition through its directory.
 */
#include <stdbool.h>

#include "asset/asset_directory.h"
#include "asset/asset_store_backend.h"
#include "core/memory.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "sdkconfig.h"

/* The first data subtype ESP-IDF leaves for an application. */
#define ASSET_PARTITION_SUBTYPE 0x40
#define ASSET_PARTITION_LABEL   "assets"

_Static_assert(ASSET_MMU_PAGE_SIZE == CONFIG_MMU_PAGE_SIZE, "asset layout must match the device MMU page size");

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

/* A small directory copy keeps enumeration independent of pack mounts and
 * holds no MMU page for the lifetime of the store. */
static asset_status_t
open_directory(void) {
    part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)ASSET_PARTITION_SUBTYPE,
                                    ASSET_PARTITION_LABEL);
    if (part == NULL) {
        return ASSET_ERR_NO_PACK;
    }
    if (part->address % ASSET_MMU_PAGE_SIZE != 0) {
        ESP_LOGE(TAG, "'%s' partition must start on a %u-byte MMU page: flash the partition table",
                 ASSET_PARTITION_LABEL, (unsigned)ASSET_MMU_PAGE_SIZE);
        return ASSET_ERR_BOUNDS;
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
asset_store_backend_directory(const asset_directory_t** opened) {
    if (!tried) {
        tried = true;
        directory_status = open_directory();
    }
    *opened = directory_status == ASSET_OK ? &directory : NULL;
    return directory_status;
}

asset_status_t
asset_store_backend_mount(const char* name, asset_pack_t* pack, uintptr_t* mapping) {
    const asset_directory_t* opened_directory;
    const asset_status_t status = asset_store_backend_directory(&opened_directory);
    if (status != ASSET_OK) {
        return status;
    }
    asset_slot_t slot;
    const asset_status_t found = asset_directory_find(opened_directory, name, &slot);
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
    ESP_LOGI(TAG, "pack %s: %u entries, %u bytes", name, (unsigned)pack->count, (unsigned)pack->size);
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
        ESP_LOGE(TAG, "pack %s: no '%s' partition: the flashed partition table is older than this firmware, flash it",
                 name, ASSET_PARTITION_LABEL);
    } else if (directory_status == ASSET_ERR_BOUNDS) {
        ESP_LOGE(TAG,
                 "pack %s: invalid directory bounds; partition and every pack must start on a %u-byte MMU page: flash "
                 "assets and the partition table",
                 name, (unsigned)ASSET_MMU_PAGE_SIZE);
    } else if (directory_status != ASSET_OK) {
        ESP_LOGE(TAG, "pack %s: the '%s' partition holds no usable pack directory (%s): flash it", name,
                 ASSET_PARTITION_LABEL, asset_status_text(status));
    } else {
        ESP_LOGE(TAG, "pack %s: %s", name, asset_status_text(status));
    }
}
