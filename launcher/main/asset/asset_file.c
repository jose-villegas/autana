#include "asset/asset_file.h"

#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#include <malloc.h>
#endif

#define FILE_ALIGN 16U

/* Not malloc(): a host test run models the board's heap by wrapping malloc,
 * and a pack the board maps from flash must not spend that budget. */
static void*
aligned_buffer(size_t size) {
    const size_t rounded = (size + FILE_ALIGN - 1U) / FILE_ALIGN * FILE_ALIGN;
#ifdef _WIN32
    return _aligned_malloc(rounded, FILE_ALIGN);
#else
    return aligned_alloc(FILE_ALIGN, rounded);
#endif
}

void
asset_file_release(void* buffer) {
#ifdef _WIN32
    _aligned_free(buffer);
#else
    free(buffer);
#endif
}

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
            bytes = aligned_buffer((size_t)length);
            if (bytes != NULL && fread(bytes, 1, (size_t)length, file) != (size_t)length) {
                asset_file_release(bytes);
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
        asset_file_release(*buffer);
        *buffer = NULL;
    }
    return status;
}
