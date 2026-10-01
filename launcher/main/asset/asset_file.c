#include "asset/asset_file.h"

#include <stdio.h>

#include "esp_heap_caps.h"

static void*
read_whole(const char* path, size_t* size) {
    FILE* file = fopen(path, "rb");
    if (file == NULL) {
        return NULL;
    }
    void* bytes = NULL;
    if (fseek(file, 0, SEEK_END) == 0) {
        const long length = ftell(file);
        if (length > 0 && fseek(file, 0, SEEK_SET) == 0) {
            bytes = heap_caps_malloc((size_t)length, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (bytes != NULL && fread(bytes, 1, (size_t)length, file) != (size_t)length) {
                heap_caps_free(bytes);
                bytes = NULL;
            }
            *size = (size_t)length;
        }
    }
    (void)fclose(file);
    return bytes;
}

asset_status_t
asset_file_open(const char* path, asset_pack_t* pack, void** buffer) {
    size_t size = 0;
    *buffer = read_whole(path, &size);
    if (*buffer == NULL) {
        *pack = (asset_pack_t){0};
        return ASSET_ERR_NO_PACK;
    }
    const asset_status_t status = asset_pack_open(pack, *buffer, size);
    if (status != ASSET_OK) {
        heap_caps_free(*buffer);
        *buffer = NULL;
    }
    return status;
}
