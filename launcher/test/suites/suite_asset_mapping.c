/* Pack mapping lifetimes must be independent for every flashed pair. */
#ifdef DEVICE_BUILD

#include <stdbool.h>
#include <stdio.h>

#include "suites.h"
#include "unity.h"

#include "asset/asset_directory.h"
#include "asset/asset_store_backend.h"

static const asset_directory_t*
fixture(void) {
    const asset_directory_t* directory;
    TEST_ASSERT_EQUAL_INT(ASSET_OK, asset_store_backend_directory(&directory));
    TEST_ASSERT_NOT_NULL(directory);
    TEST_ASSERT_GREATER_THAN_UINT32(1, directory->count);
    return directory;
}

static void
test_releasing_any_pack_preserves_every_other_pack(void) {
    const asset_directory_t* directory = fixture();
    const char* name_a = NULL;
    const char* name_b = NULL;
    uintptr_t mapping_a = 0;
    uintptr_t mapping_b = 0;
    bool held_a = false;
    bool held_b = false;
    asset_status_t status = ASSET_OK;
    for (uint32_t a = 0; a < directory->count; a++) {
        for (uint32_t b = 0; b < directory->count; b++) {
            if (a == b) {
                continue;
            }
            asset_slot_t slot;
            status = asset_directory_entry(directory, a, &name_a, &slot);
            if (status != ASSET_OK) {
                goto cleanup;
            }
            status = asset_directory_entry(directory, b, &name_b, &slot);
            if (status != ASSET_OK) {
                goto cleanup;
            }
            asset_pack_t pack_a;
            asset_pack_t pack_b;
            status = asset_store_backend_mount(name_a, &pack_a, &mapping_a);
            if (status != ASSET_OK) {
                goto cleanup;
            }
            held_a = true;
            status = asset_store_backend_mount(name_b, &pack_b, &mapping_b);
            if (status != ASSET_OK) {
                goto cleanup;
            }
            held_b = true;
            const uint32_t crc = asset_crc32(pack_b.base, pack_b.size);
            asset_store_backend_unmount(mapping_a);
            held_a = false;
            asset_pack_t checked;
            status = asset_pack_open(&checked, pack_b.base, pack_b.size);
            if (status == ASSET_OK && asset_crc32(pack_b.base, pack_b.size) != crc) {
                status = ASSET_ERR_CRC;
            }
            if (status != ASSET_OK) {
                goto cleanup;
            }
            asset_store_backend_unmount(mapping_b);
            held_b = false;
        }
    }
cleanup:
    if (held_a) {
        asset_store_backend_unmount(mapping_a);
    }
    if (held_b) {
        asset_store_backend_unmount(mapping_b);
    }
    if (status != ASSET_OK) {
        printf("mapping pair: release %s, preserve %s\n", name_a, name_b);
    }
    TEST_ASSERT_EQUAL_INT(ASSET_OK, status);
}

void
run_asset_mapping_suite(void) {
    RUN_TEST(test_releasing_any_pack_preserves_every_other_pack);
}

SUITE_REGISTER(run_asset_mapping_suite);
#endif
