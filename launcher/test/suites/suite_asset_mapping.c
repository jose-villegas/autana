/* Pack mapping lifetimes must be independent for every flashed pair. */
#ifdef DEVICE_BUILD

#include <stdio.h>

#include "suites.h"
#include "unity.h"

#include "asset/asset_directory.h"
#include "asset/asset_store_backend.h"
#include "hash/crc32.h"

static const asset_directory_t*
fixture(void) {
    const asset_directory_t* directory;
    TEST_ASSERT_EQUAL_INT(ASSET_OK, asset_store_backend_directory(&directory));
    TEST_ASSERT_NOT_NULL(directory);
    TEST_ASSERT_GREATER_THAN_UINT32(1, directory->count);
    return directory;
}

static asset_status_t
check_mapping_pair(const char* name_a, const char* name_b) {
    asset_pack_t pack_a;
    asset_pack_t pack_b;
    uintptr_t mapping_a;
    uintptr_t mapping_b;
    asset_status_t status = asset_store_backend_mount(name_a, &pack_a, &mapping_a);
    if (status != ASSET_OK) {
        return status;
    }
    status = asset_store_backend_mount(name_b, &pack_b, &mapping_b);
    if (status != ASSET_OK) {
        asset_store_backend_unmount(mapping_a);
        return status;
    }
    const uint32_t crc = crc32_update(0, pack_b.base, pack_b.size);
    asset_store_backend_unmount(mapping_a);
    asset_pack_t checked;
    status = asset_pack_open(&checked, pack_b.base, pack_b.size);
    if (status == ASSET_OK && crc32_update(0, pack_b.base, pack_b.size) != crc) {
        status = ASSET_ERR_CRC;
    }
    asset_store_backend_unmount(mapping_b);
    return status;
}

static void
test_releasing_any_pack_preserves_every_other_pack(void) {
    const asset_directory_t* directory = fixture();
    for (uint32_t a = 0; a < directory->count; a++) {
        const char* name_a;
        asset_slot_t slot;
        TEST_ASSERT_EQUAL_INT(ASSET_OK, asset_directory_entry(directory, a, &name_a, &slot));
        for (uint32_t b = 0; b < directory->count; b++) {
            if (a == b) {
                continue;
            }
            const char* name_b;
            TEST_ASSERT_EQUAL_INT(ASSET_OK, asset_directory_entry(directory, b, &name_b, &slot));
            const asset_status_t status = check_mapping_pair(name_a, name_b);
            if (status != ASSET_OK) {
                printf("mapping pair: release %s, preserve %s\n", name_a, name_b);
            }
            TEST_ASSERT_EQUAL_INT(ASSET_OK, status);
        }
    }
}

void
run_asset_mapping_suite(void) {
    RUN_TEST(test_releasing_any_pack_preserves_every_other_pack);
}

SUITE_REGISTER(run_asset_mapping_suite);
#endif
