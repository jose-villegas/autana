/*
 * Portable suite: the pack directory (asset/asset_directory.h), and on a
 * host the store's counted mounts (asset/asset_store.h) over pack files
 * this suite writes, so no check leans on what the tree's packs hold.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "suites.h"
#include "unity.h"

#include "asset/asset_directory.h"
#include "asset/asset_pack.h"
#include "asset/asset_store.h"
#include "test_pack.h"

#ifndef DEVICE_BUILD
#include "test_asset_dir.h"
#endif

#define ROWS            2
#define DIRECTORY_BYTES (ASSET_DIRECTORY_HEADER_SIZE + (ROWS * ASSET_DIRECTORY_ROW_SIZE))
#define REGION          (4U * ASSET_PACK_ALIGN)

static uint8_t*
row(uint8_t* directory, int index) {
    return directory + ASSET_DIRECTORY_HEADER_SIZE + (index * ASSET_DIRECTORY_ROW_SIZE);
}

static void
seal(uint8_t* directory) {
    test_pack_put32(directory + 4, asset_crc32(directory + 8, DIRECTORY_BYTES - 8));
}

/* Pack "one" at the first sector after the directory, "two" two sectors on. */
static uint8_t*
make_directory(void) {
    uint8_t* directory = calloc(1, DIRECTORY_BYTES);
    TEST_ASSERT_NOT_NULL(directory);
    memcpy(directory, ASSET_DIRECTORY_MAGIC, 4);
    test_pack_put32(directory + 8, ASSET_DIRECTORY_VERSION);
    test_pack_put32(directory + 12, ROWS);
    memcpy(row(directory, 0), "one", 3);
    test_pack_put32(row(directory, 0) + 32, ASSET_PACK_ALIGN);
    test_pack_put32(row(directory, 0) + 36, 100);
    memcpy(row(directory, 1), "two", 3);
    test_pack_put32(row(directory, 1) + 32, 3U * ASSET_PACK_ALIGN);
    test_pack_put32(row(directory, 1) + 36, ASSET_PACK_ALIGN);
    seal(directory);
    return directory;
}

static asset_status_t
open_sealed(uint8_t* directory) {
    seal(directory);
    asset_directory_t opened;
    return asset_directory_open(&opened, directory, DIRECTORY_BYTES, REGION);
}

static void
test_a_directory_opens_and_finds_each_pack_by_name(void) {
    uint8_t* directory = make_directory();
    asset_directory_t opened;
    asset_slot_t slot;
    TEST_ASSERT_EQUAL_UINT32(DIRECTORY_BYTES, asset_directory_size(directory, ASSET_DIRECTORY_HEADER_SIZE));
    TEST_ASSERT_EQUAL_INT(ASSET_OK, asset_directory_open(&opened, directory, DIRECTORY_BYTES, REGION));
    TEST_ASSERT_EQUAL_INT(ASSET_OK, asset_directory_find(&opened, "two", &slot));
    TEST_ASSERT_EQUAL_UINT32(3U * ASSET_PACK_ALIGN, slot.offset);
    TEST_ASSERT_EQUAL_UINT32(ASSET_PACK_ALIGN, slot.size);
    TEST_ASSERT_EQUAL_INT(ASSET_OK, asset_directory_find(&opened, "one", &slot));
    TEST_ASSERT_EQUAL_UINT32(100, slot.size);
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_NOT_FOUND, asset_directory_find(&opened, "on", &slot));
    free(directory);
}

static void
test_a_bad_magic_version_or_checksum_is_refused(void) {
    uint8_t* directory = make_directory();
    asset_directory_t opened;
    directory[0] = 'X';
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_MAGIC, asset_directory_open(&opened, directory, DIRECTORY_BYTES, REGION));
    TEST_ASSERT_EQUAL_UINT32(0, asset_directory_size(directory, DIRECTORY_BYTES));
    directory[0] = 'A';
    test_pack_put32(directory + 8, ASSET_DIRECTORY_VERSION + 1U);
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_VERSION, open_sealed(directory));
    test_pack_put32(directory + 8, ASSET_DIRECTORY_VERSION);
    seal(directory);
    row(directory, 1)[2] = 'x';
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_CRC, asset_directory_open(&opened, directory, DIRECTORY_BYTES, REGION));
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_TRUNCATED, asset_directory_open(&opened, directory, DIRECTORY_BYTES - 1, REGION));
    TEST_ASSERT_NULL(opened.base);
    free(directory);
}

/* Each edit is checked with a good checksum, so the row check itself refuses it. */
static void
test_a_row_out_of_range_misaligned_or_overlapping_is_refused(void) {
    const struct {
        int row;
        int field;
        uint32_t value;
    } cases[] = {
        {1, 36, (2U * ASSET_PACK_ALIGN) + 1U},  /* ends past the region */
        {1, 32, REGION},                        /* starts at its end */
        {1, 32, (2U * ASSET_PACK_ALIGN) + 16U}, /* not on a sector */
        {0, 32, 0},                             /* over the directory */
        {1, 32, ASSET_PACK_ALIGN},              /* over pack one */
    };

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        uint8_t* directory = make_directory();
        test_pack_put32(row(directory, cases[i].row) + cases[i].field, cases[i].value);
        TEST_ASSERT_EQUAL_INT_MESSAGE(ASSET_ERR_BOUNDS, open_sealed(directory), "case");
        free(directory);
    }
}

static void
test_two_packs_with_one_name_or_a_name_with_no_room_for_its_nul_are_refused(void) {
    uint8_t* directory = make_directory();
    memcpy(row(directory, 1), "one", 3);
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_DUPLICATE, open_sealed(directory));
    memset(row(directory, 1), 'n', ASSET_NAME_MAX);
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_BOUNDS, open_sealed(directory));
    memset(row(directory, 1), 0, ASSET_NAME_MAX);
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_BOUNDS, open_sealed(directory));
    free(directory);
}

#ifndef DEVICE_BUILD
#define PACK_BYTES 96
#define NOTE_TYPE  ASSET_TYPE('N', 'O', 'T', 'E')
#define NOTE_BYTES 16

/* The packs go to the working directory, named after this suite. */
static void
pack_path(char* path, size_t size, const char* name) {
    (void)snprintf(path, size, "./%s.apak", name);
}

/* Writes pack `name`: one NOTE entry holding `note`. `damage` flips a byte
 * after the header, which its CRC covers. */
static void
write_pack(const char* name, const char* note, int damage) {
    uint8_t* bytes = malloc(PACK_BYTES);
    TEST_ASSERT_NOT_NULL(bytes);
    test_pack_t pack = test_pack_begin(bytes, PACK_BYTES, 1);
    uint8_t* text = test_pack_add(&pack, "note", NOTE_TYPE, NOTE_BYTES);
    (void)strncpy((char*)text, note, NOTE_BYTES - 1);
    const uint32_t size = test_pack_finish(&pack);
    text[0] ^= (uint8_t)damage;
    char path[64];
    pack_path(path, sizeof path, name);
    test_write_file(path, bytes, size);
    free(bytes);
}

static void
remove_pack(const char* name) {
    char path[64];
    pack_path(path, sizeof path, name);
    (void)remove(path);
}

static const char*
note_of(const asset_pack_t* pack) {
    asset_view_t view;
    TEST_ASSERT_NOT_NULL(pack);
    TEST_ASSERT_EQUAL_INT(ASSET_OK, asset_pack_find(pack, "note", NOTE_TYPE, &view));
    return (const char*)view.data;
}

static void
test_two_packs_mount_alone_and_a_bad_checksum_in_one_leaves_the_other_usable(void) {
    test_asset_dir_use(".");
    write_pack("suite_pack_a", "first", 0);
    write_pack("suite_pack_b", "second", 1);
    const asset_pack_t* a = asset_store_pack("suite_pack_a");
    TEST_ASSERT_NULL(asset_store_pack("suite_pack_b"));
    TEST_ASSERT_NULL(asset_store_pack("suite_pack_missing"));
    TEST_ASSERT_EQUAL_STRING("first", note_of(a));
    write_pack("suite_pack_b", "second", 0);
    const asset_pack_t* b = asset_store_pack("suite_pack_b");
    TEST_ASSERT_EQUAL_STRING("second", note_of(b));
    TEST_ASSERT_EQUAL_STRING("first", note_of(a));
    asset_store_release("suite_pack_a");
    asset_store_release("suite_pack_b");
    remove_pack("suite_pack_a");
    remove_pack("suite_pack_b");
    test_asset_dir_restore();
}

/* A counted mount is read once: a damaged file goes unseen until the last
 * use is released and the next mount reads it again. */
static void
test_a_pack_is_read_on_its_first_use_and_again_only_after_its_last_release(void) {
    test_asset_dir_use(".");
    write_pack("suite_pack_a", "first", 0);
    const asset_pack_t* a = asset_store_pack("suite_pack_a");
    write_pack("suite_pack_a", "first", 1);
    TEST_ASSERT_EQUAL_PTR(a, asset_store_pack("suite_pack_a"));
    asset_store_release("suite_pack_a");
    TEST_ASSERT_EQUAL_STRING("first", note_of(a));
    asset_store_release("suite_pack_a");
    TEST_ASSERT_NULL(asset_store_pack("suite_pack_a"));
    remove_pack("suite_pack_a");
    test_asset_dir_restore();
}

static void
test_a_mount_past_the_store_s_slots_is_refused_until_one_is_released(void) {
    test_asset_dir_use(".");
    char names[ASSET_STORE_MOUNTS_MAX + 1][24];
    for (int i = 0; i <= ASSET_STORE_MOUNTS_MAX; i++) {
        (void)snprintf(names[i], sizeof names[i], "suite_pack_%d", i);
        write_pack(names[i], names[i], 0);
    }
    /* Packs the run keeps mounted (the engine's artwork) hold their slots. */
    const int free_slots = ASSET_STORE_MOUNTS_MAX - asset_store_mounted();
    TEST_ASSERT_GREATER_THAN_INT(0, free_slots);
    for (int i = 0; i < free_slots; i++) {
        TEST_ASSERT_NOT_NULL(asset_store_pack(names[i]));
    }
    TEST_ASSERT_EQUAL_INT(ASSET_STORE_MOUNTS_MAX, asset_store_mounted());
    TEST_ASSERT_NULL(asset_store_pack(names[free_slots]));
    asset_store_release(names[0]);
    TEST_ASSERT_EQUAL_STRING(names[free_slots], note_of(asset_store_pack(names[free_slots])));
    for (int i = 1; i <= free_slots; i++) {
        asset_store_release(names[i]);
    }
    for (int i = 0; i <= ASSET_STORE_MOUNTS_MAX; i++) {
        remove_pack(names[i]);
    }
    test_asset_dir_restore();
}
#endif

void
suite_asset_store(void) {
    RUN_TEST(test_a_directory_opens_and_finds_each_pack_by_name);
    RUN_TEST(test_a_bad_magic_version_or_checksum_is_refused);
    RUN_TEST(test_a_row_out_of_range_misaligned_or_overlapping_is_refused);
    RUN_TEST(test_two_packs_with_one_name_or_a_name_with_no_room_for_its_nul_are_refused);
#ifndef DEVICE_BUILD
    RUN_TEST(test_two_packs_mount_alone_and_a_bad_checksum_in_one_leaves_the_other_usable);
    RUN_TEST(test_a_pack_is_read_on_its_first_use_and_again_only_after_its_last_release);
    RUN_TEST(test_a_mount_past_the_store_s_slots_is_refused_until_one_is_released);
#endif
}

SUITE_REGISTER(suite_asset_store);
