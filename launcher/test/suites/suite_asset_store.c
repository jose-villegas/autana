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
#include "asset/asset_directory_format.h"
#include "asset/asset_pack.h"
#include "asset/asset_store.h"
#include "test_pack.h"

#ifndef DEVICE_BUILD
#include "hash/crc32.h"
#include "shell/shell_system.h"
#include "test_asset_dir.h"

#include "../asset_examples/load_pack.c"
#include "../asset_examples/load_pack_async.c"
#include "asset/asset_store_internal.h"
#include "asset/asset_store_shell.h"
#endif

#define ROWS            2
#define DIRECTORY_BYTES (ASSET_DIRECTORY_HEADER_SIZE + (ROWS * ASSET_DIRECTORY_ROW_SIZE))
#define REGION          (4U * ASSET_MMU_PAGE_SIZE)

static uint8_t*
row(uint8_t* directory, int index) {
    return directory + ASSET_DIRECTORY_HEADER_SIZE + (index * ASSET_DIRECTORY_ROW_SIZE);
}

static void
seal(uint8_t* directory) {
    test_pack_put32(directory + 4, crc32_update(0, directory + 8, DIRECTORY_BYTES - 8));
}

/* Pack "one" at the first MMU page after the directory, "two" two MMU pages on. */
static uint8_t*
make_directory(void) {
    uint8_t* directory = calloc(1, DIRECTORY_BYTES);
    TEST_ASSERT_NOT_NULL(directory);
    memcpy(directory, ASSET_DIRECTORY_MAGIC, 4);
    test_pack_put32(directory + 8, ASSET_DIRECTORY_VERSION);
    test_pack_put32(directory + 12, ROWS);
    memcpy(row(directory, 0), "one", 3);
    test_pack_put32(row(directory, 0) + 32, ASSET_MMU_PAGE_SIZE);
    test_pack_put32(row(directory, 0) + 36, 100);
    memcpy(row(directory, 1), "two", 3);
    test_pack_put32(row(directory, 1) + 32, 3U * ASSET_MMU_PAGE_SIZE);
    test_pack_put32(row(directory, 1) + 36, ASSET_MMU_PAGE_SIZE);
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
    TEST_ASSERT_EQUAL_UINT32(3U * ASSET_MMU_PAGE_SIZE, slot.offset);
    TEST_ASSERT_EQUAL_UINT32(ASSET_MMU_PAGE_SIZE, slot.size);
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
        {1, 36, (2U * ASSET_MMU_PAGE_SIZE) + 1U},                         /* ends past the region */
        {1, 32, REGION},                                                  /* starts at its end */
        {1, 32, (2U * ASSET_MMU_PAGE_SIZE) + 16U},                        /* not on a MMU page */
        {1, 32, (2U * ASSET_MMU_PAGE_SIZE) + (ASSET_MMU_PAGE_SIZE / 2U)}, /* sector aligned, off a page */
        {0, 32, 0},                                                       /* over the directory */
        {1, 32, ASSET_MMU_PAGE_SIZE},                                     /* over pack one */
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
static unsigned failure_logs;

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
write_pack(const char* name, const char* note, int damage, uint32_t payload_bytes) {
    const uint32_t capacity =
        ASSET_PACK_HEADER_SIZE + ASSET_PACK_ENTRY_SIZE + (uint32_t)test_pack_aligned(payload_bytes);
    uint8_t* bytes = malloc(capacity);
    TEST_ASSERT_NOT_NULL(bytes);
    test_pack_t pack = test_pack_begin(bytes, capacity, 1);
    uint8_t* text = test_pack_add(&pack, "note", NOTE_TYPE, payload_bytes);
    (void)strncpy((char*)text, note, payload_bytes - 1U);
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
    write_pack("suite_pack_a", "first", 0, NOTE_BYTES);
    write_pack("suite_pack_b", "second", 1, NOTE_BYTES);
    const asset_pack_t* a = asset_store_load("suite_pack_a");
    TEST_ASSERT_NULL(asset_store_load("suite_pack_b"));
    TEST_ASSERT_NULL(asset_store_load("suite_pack_missing"));
    TEST_ASSERT_EQUAL_STRING("first", note_of(a));
    write_pack("suite_pack_b", "second", 0, NOTE_BYTES);
    const asset_pack_t* b = asset_store_load("suite_pack_b");
    TEST_ASSERT_EQUAL_STRING("second", note_of(b));
    TEST_ASSERT_EQUAL_STRING("first", note_of(a));
    asset_store_release("suite_pack_a");
    asset_store_release("suite_pack_b");
    remove_pack("suite_pack_a");
    remove_pack("suite_pack_b");
}

static void
test_the_load_pack_example_reads_an_entry_and_releases_the_pack(void) {
    test_asset_dir_use(".");
    write_pack("suite_pack_a", "first", 0, NOTE_BYTES);
    uint32_t size = 0;
    const int mounted = asset_store_mounted();
    TEST_ASSERT_EQUAL_INT(ASSET_OK, load_pack_example("suite_pack_a", "note", NOTE_TYPE, &size));
    TEST_ASSERT_EQUAL_UINT32(NOTE_BYTES, size);
    TEST_ASSERT_EQUAL_INT(mounted, asset_store_mounted());
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_NOT_FOUND, load_pack_example("suite_pack_a", "absent", NOTE_TYPE, &size));
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_TYPE,
                          load_pack_example("suite_pack_a", "note", ASSET_TYPE('N', 'O', 'P', 'E'), &size));
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_NO_PACK, load_pack_example("suite_pack_missing", "note", NOTE_TYPE, &size));
    remove_pack("suite_pack_a");
}

/* A counted mount is read once: a damaged file goes unseen until the last
 * use is released and the next mount reads it again. */
static void
test_a_pack_is_read_on_its_first_use_and_again_only_after_its_last_release(void) {
    test_asset_dir_use(".");
    write_pack("suite_pack_a", "first", 0, NOTE_BYTES);
    const asset_pack_t* a = asset_store_load("suite_pack_a");
    write_pack("suite_pack_a", "first", 1, NOTE_BYTES);
    TEST_ASSERT_EQUAL_PTR(a, asset_store_load("suite_pack_a"));
    asset_store_release("suite_pack_a");
    TEST_ASSERT_EQUAL_STRING("first", note_of(a));
    asset_store_release("suite_pack_a");
    TEST_ASSERT_NULL(asset_store_load("suite_pack_a"));
    remove_pack("suite_pack_a");
}

static void
test_a_mount_past_the_store_s_slots_is_refused_until_one_is_released(void) {
    test_asset_dir_use(".");
    char names[ASSET_STORE_MOUNTS_MAX + 1][24];
    for (int i = 0; i <= ASSET_STORE_MOUNTS_MAX; i++) {
        (void)snprintf(names[i], sizeof names[i], "suite_pack_%d", i);
        write_pack(names[i], names[i], 0, NOTE_BYTES);
    }
    /* Packs the run keeps mounted (the engine's artwork) hold their slots. */
    const int free_slots = ASSET_STORE_MOUNTS_MAX - asset_store_mounted();
    TEST_ASSERT_GREATER_THAN_INT(0, free_slots);
    for (int i = 0; i < free_slots; i++) {
        TEST_ASSERT_NOT_NULL(asset_store_load(names[i]));
    }
    TEST_ASSERT_EQUAL_INT(ASSET_STORE_MOUNTS_MAX, asset_store_mounted());
    failure_logs = 0;
    TEST_ASSERT_NULL(asset_store_load(names[free_slots]));
    TEST_ASSERT_EQUAL_UINT(1, failure_logs);
    failure_logs = 0;
    handle_t overflow = asset_store_load_async(names[free_slots], NULL, HANDLE_NONE);
    shell_systems_poll(0);
    TEST_ASSERT_EQUAL_INT(PENDING_FAILED, asset_store_state(overflow));
    TEST_ASSERT_EQUAL_UINT(1, failure_logs);
    asset_store_cancel(overflow);
    asset_store_release(names[0]);
    TEST_ASSERT_EQUAL_STRING(names[free_slots], note_of(asset_store_load(names[free_slots])));
    for (int i = 1; i <= free_slots; i++) {
        asset_store_release(names[i]);
    }
    for (int i = 0; i <= ASSET_STORE_MOUNTS_MAX; i++) {
        remove_pack(names[i]);
    }
}

static int completions;
static pending_t completed_state;
static handle_t completed_request, completed_owner;
static bool completed_loaded;

static void
loaded_done(handle_t request, pending_t state, handle_t owner) {
    completed_state = state;
    completed_request = request;
    completed_owner = owner;
    completed_loaded = asset_store_loaded(request) != NULL;
    completions++;
}

static void
test_async_delivery_is_deferred_and_once(void) {
    test_asset_dir_use(".");
    write_pack("suite_async", "async", 0, NOTE_BYTES);
    completions = 0;
    handle_t load = asset_store_load_async("suite_async", loaded_done, HANDLE_NONE);
    TEST_ASSERT_EQUAL_INT(PENDING_WAIT, asset_store_state(load));
    TEST_ASSERT_EQUAL_INT(0, completions);
    shell_systems_poll(0);
    TEST_ASSERT_EQUAL_INT(PENDING_READY, completed_state);
    TEST_ASSERT_TRUE(completed_loaded);
    TEST_ASSERT_TRUE(handle_equal(load, completed_request));
    TEST_ASSERT_TRUE(handle_is_none(completed_owner));
    TEST_ASSERT_EQUAL_INT(1, completions);
    shell_systems_poll(0);
    TEST_ASSERT_EQUAL_INT(1, completions);
    asset_store_release("suite_async");
    remove_pack("suite_async");
}

#define ASYNC_PAYLOAD_BYTES (2U * ASSET_STORE_STEP_BYTES)
#define ASYNC_NAME          "suite_async_large"
#define ASYNC_STEPS         3U

static void
test_async_known_size_takes_bounded_steps_and_shares_one_mount(void) {
    test_asset_dir_use(".");
    write_pack(ASYNC_NAME, "", 0, ASYNC_PAYLOAD_BYTES);
    const int before = asset_store_mounted();
    completions = 0;
    handle_t first = asset_store_load_async(ASYNC_NAME, loaded_done, HANDLE_NONE);
    handle_t second = asset_store_load_async(ASYNC_NAME, loaded_done, HANDLE_NONE);
    for (uint32_t step = 1; step < ASYNC_STEPS; step++) {
        shell_systems_poll(0);
        TEST_ASSERT_EQUAL_INT(PENDING_WAIT, asset_store_state(first));
        TEST_ASSERT_NULL(asset_store_loaded(first));
        TEST_ASSERT_EQUAL_INT(0, completions);
    }
    shell_systems_poll(0);
    TEST_ASSERT_EQUAL_INT(2, completions);
    TEST_ASSERT_EQUAL_PTR(asset_store_loaded(first), asset_store_loaded(second));
    TEST_ASSERT_EQUAL_INT(before + 1, asset_store_mounted());
    asset_store_release(ASYNC_NAME);
    TEST_ASSERT_EQUAL_INT(PENDING_NONE, asset_store_state(first));
    TEST_ASSERT_EQUAL_INT(PENDING_READY, asset_store_state(second));
    asset_store_release(ASYNC_NAME);
    TEST_ASSERT_EQUAL_INT(before, asset_store_mounted());
    remove_pack(ASYNC_NAME);
}

static void
test_cancel_frees_partial_mount_and_reuses_a_generation(void) {
    test_asset_dir_use(".");
    write_pack(ASYNC_NAME, "", 0, ASYNC_PAYLOAD_BYTES);
    const int before = asset_store_mounted();
    completions = 0;
    handle_t old = asset_store_load_async(ASYNC_NAME, loaded_done, HANDLE_NONE);
    shell_systems_poll(0);
    asset_store_cancel(old);
    TEST_ASSERT_EQUAL_INT(PENDING_CANCELLED, asset_store_state(old));
    handle_t load = asset_store_load_async(ASYNC_NAME, NULL, HANDLE_NONE);
    TEST_ASSERT_FALSE(handle_equal(old, load));
    TEST_ASSERT_EQUAL_INT(PENDING_NONE, asset_store_state(old));
    shell_systems_poll(0);
    shell_systems_app_exit();
    shell_systems_poll(0);
    TEST_ASSERT_EQUAL_INT(0, completions);
    TEST_ASSERT_EQUAL_INT(before, asset_store_mounted());
    TEST_ASSERT_NULL(asset_store_loaded(load));
    TEST_ASSERT_NOT_NULL(asset_store_load(ASYNC_NAME));
    asset_store_release(ASYNC_NAME);
    remove_pack(ASYNC_NAME);
}

static void
test_token_cancel_silences_partial_and_already_ready_callbacks(void) {
    test_asset_dir_use(".");
    write_pack(ASYNC_NAME, "", 0, ASYNC_PAYLOAD_BYTES);
    completions = 0;
    handle_t token = pending_token_new();
    handle_t load = asset_store_load_async(ASYNC_NAME, loaded_done, token);
    shell_systems_poll(0);
    pending_token_cancel(token);
    for (uint32_t i = 0; i < ASYNC_STEPS; i++) {
        shell_systems_poll(0);
    }
    TEST_ASSERT_EQUAL_INT(PENDING_CANCELLED, asset_store_state(load));
    TEST_ASSERT_EQUAL_INT(0, completions);
    TEST_ASSERT_NOT_NULL(asset_store_load(ASYNC_NAME));
    load = asset_store_load_async(ASYNC_NAME, loaded_done, HANDLE_NONE);
    TEST_ASSERT_EQUAL_INT(PENDING_READY, asset_store_state(load));
    asset_store_cancel(load);
    shell_systems_poll(0);
    TEST_ASSERT_EQUAL_INT(0, completions);
    asset_store_release(ASYNC_NAME);
    asset_store_release(ASYNC_NAME);
    remove_pack(ASYNC_NAME);
}

static void
test_failure_is_pollable_once_delivered_and_oldest_is_reclaimed(void) {
    test_asset_dir_use(".");
    write_pack("suite_async_bad", "bad", 1, NOTE_BYTES);
    completions = 0;
    handle_t failed = asset_store_load_async("suite_async_bad", loaded_done, HANDLE_NONE);
    TEST_ASSERT_EQUAL_INT(0, completions);
    shell_systems_poll(0);
    TEST_ASSERT_EQUAL_INT(PENDING_FAILED, asset_store_state(failed));
    TEST_ASSERT_EQUAL_INT(1, completions);
    shell_systems_poll(0);
    TEST_ASSERT_EQUAL_INT(1, completions);
    TEST_ASSERT_NULL(asset_store_load("suite_async_bad"));
    for (uint32_t i = 0; i < ASSET_STORE_REQUESTS_MAX; i++) {
        handle_t next = asset_store_load_async("suite_async_bad", NULL, HANDLE_NONE);
        TEST_ASSERT_FALSE(handle_is_none(next));
        shell_systems_poll(0);
        TEST_ASSERT_EQUAL_INT(PENDING_FAILED, asset_store_state(next));
    }
    TEST_ASSERT_EQUAL_INT(PENDING_NONE, asset_store_state(failed));
    shell_systems_app_exit();
    remove_pack("suite_async_bad");
}

static void
test_async_example_releases_or_cancels_its_visit(void) {
    test_asset_dir_use(".");
    write_pack("suite_async", "example", 0, NOTE_BYTES);
    const int before = asset_store_mounted();
    load_pack_example_t visit;
    load_pack_enter(&visit, "suite_async");
    TEST_ASSERT_NULL(load_pack_frame(&visit));
    shell_systems_poll(0);
    TEST_ASSERT_EQUAL_STRING("example", note_of(load_pack_frame(&visit)));
    load_pack_exit(&visit);
    TEST_ASSERT_EQUAL_INT(before, asset_store_mounted());
    load_pack_enter(&visit, "suite_async");
    load_pack_exit(&visit);
    shell_systems_poll(0);
    TEST_ASSERT_EQUAL_INT(before, asset_store_mounted());
    remove_pack("suite_async");
}

static void
test_token_generation_wrap_retires_the_slot(void) {
    handle_t first = pending_token_new();
    handle_t token = first;
    const uint32_t slot = handle_slot(first);
    const uint32_t start = handle_generation(first);
    for (uint32_t generation = start; generation < HANDLE_GENERATION_MAX; generation++) {
        pending_token_cancel(token);
        token = pending_token_new();
        TEST_ASSERT_EQUAL_UINT32(slot, handle_slot(token));
    }
    pending_token_cancel(token);
    token = pending_token_new();
    TEST_ASSERT_NOT_EQUAL(slot, handle_slot(token));
    TEST_ASSERT_FALSE(pending_owner_alive(first));
    pending_token_cancel(token);
}

static handle_t callback_child;

static void
spawn_done(handle_t request, pending_t state, handle_t owner) {
    loaded_done(request, state, owner);
    callback_child = asset_store_load_async("suite_async", loaded_done, HANDLE_NONE);
}

static void
test_callback_requests_wait_a_pass_and_blocking_load_never_delivers_them(void) {
    test_asset_dir_use(".");
    write_pack("suite_async", "callback", 0, NOTE_BYTES);
    completions = 0;
    handle_t first = asset_store_load_async("suite_async", spawn_done, HANDLE_NONE);
    TEST_ASSERT_NOT_NULL(asset_store_load("suite_async"));
    TEST_ASSERT_EQUAL_INT(0, completions);
    shell_systems_poll(0);
    TEST_ASSERT_EQUAL_INT(1, completions);
    TEST_ASSERT_EQUAL_INT(PENDING_READY, asset_store_state(first));
    TEST_ASSERT_EQUAL_INT(PENDING_READY, asset_store_state(callback_child));
    shell_systems_poll(0);
    TEST_ASSERT_EQUAL_INT(2, completions);
    asset_store_release("suite_async");
    asset_store_release("suite_async");
    asset_store_release("suite_async");
    remove_pack("suite_async");
}

static handle_t*
fill_request_pool(const char* name) {
    handle_t* loads = malloc(ASSET_STORE_REQUESTS_MAX * sizeof(*loads));
    TEST_ASSERT_NOT_NULL(loads);
    for (uint32_t i = 0; i < ASSET_STORE_REQUESTS_MAX; i++) {
        loads[i] = asset_store_load_async(name, NULL, HANDLE_NONE);
        TEST_ASSERT_FALSE(handle_is_none(loads[i]));
    }
    return loads;
}

static void
test_request_pool_limits_pending_work_and_cancel_returns_a_slot(void) {
    handle_t* loads = malloc(sizeof(*loads) * ASSET_STORE_REQUESTS_MAX);
    TEST_ASSERT_NOT_NULL(loads);
    for (uint32_t i = 0; i < ASSET_STORE_REQUESTS_MAX; i++) {
        loads[i] = asset_store_load_async("suite_not_opened", NULL, HANDLE_NONE);
        TEST_ASSERT_FALSE(handle_is_none(loads[i]));
    }
    TEST_ASSERT_TRUE(handle_is_none(asset_store_load_async("suite_not_opened", NULL, HANDLE_NONE)));
    const handle_t stale = loads[0];
    asset_store_cancel(stale);
    loads[0] = asset_store_load_async("suite_not_opened", NULL, HANDLE_NONE);
    TEST_ASSERT_FALSE(handle_is_none(loads[0]));
    TEST_ASSERT_EQUAL_INT(PENDING_NONE, asset_store_state(stale));
    for (uint32_t i = 0; i < ASSET_STORE_REQUESTS_MAX; i++) {
        asset_store_cancel(loads[i]);
    }
    free(loads);
}

static void
test_handle_fields_and_generation_retirement(void) {
    handle_t h = handle_make(HANDLE_OWNER_TOKEN, HANDLE_SLOT_MAX, HANDLE_GENERATION_MAX);
    TEST_ASSERT_EQUAL_UINT32(HANDLE_OWNER_TOKEN, handle_owner(h));
    TEST_ASSERT_EQUAL_UINT32(HANDLE_SLOT_MAX, handle_slot(h));
    TEST_ASSERT_EQUAL_UINT32(HANDLE_GENERATION_MAX, handle_generation(h));
    TEST_ASSERT_TRUE(handle_equal(h, h));
    TEST_ASSERT_TRUE(handle_is_none(HANDLE_NONE));
    TEST_ASSERT_FALSE(handle_generation_next(HANDLE_GENERATION_MAX));
    TEST_ASSERT_TRUE(handle_generation_next(HANDLE_GENERATION_MAX - 1U));
}

void __real_asset_store_backend_report(const char* name, asset_status_t status);

void
__wrap_asset_store_backend_report(const char* name, asset_status_t status) {
    failure_logs++;
    __real_asset_store_backend_report(name, status);
}

static void
poll_complete(handle_t load) {
    for (uint32_t i = 0; i < ASYNC_STEPS && asset_store_state(load) == PENDING_WAIT; i++) {
        shell_systems_poll(0);
    }
    TEST_ASSERT_NOT_EQUAL(PENDING_WAIT, asset_store_state(load));
}

static void
test_cancel_ready_retains_every_use_and_dead_owner_does_too(void) {
    test_asset_dir_use(".");
    write_pack(ASYNC_NAME, "retained", 0, NOTE_BYTES);
    const int before = asset_store_mounted();
    handle_t token = pending_token_new();
    handle_t first = asset_store_load_async(ASYNC_NAME, NULL, token);
    poll_complete(first);
    handle_t second = asset_store_load_async(ASYNC_NAME, NULL, HANDLE_NONE);
    const asset_pack_t* pack = asset_store_loaded(second);
    asset_store_cancel(first);
    TEST_ASSERT_EQUAL_INT(PENDING_READY, asset_store_state(first));
    TEST_ASSERT_EQUAL_INT(before + 1, asset_store_mounted());
    pending_token_cancel(token);
    shell_systems_poll(0);
    asset_store_release(ASYNC_NAME);
    TEST_ASSERT_EQUAL_INT(before + 1, asset_store_mounted());
    TEST_ASSERT_EQUAL_PTR(pack, asset_store_loaded(second));
    TEST_ASSERT_EQUAL_STRING("retained", note_of(pack));
    asset_store_release(ASYNC_NAME);
    TEST_ASSERT_EQUAL_INT(before, asset_store_mounted());
    remove_pack(ASYNC_NAME);
}

static void
test_cancel_wait_failed_and_blocking_holder_preserve_other_uses(void) {
    test_asset_dir_use(".");
    write_pack(ASYNC_NAME, "retained", 0, ASYNC_PAYLOAD_BYTES);
    const int before = asset_store_mounted();
    handle_t first = asset_store_load_async(ASYNC_NAME, NULL, HANDLE_NONE);
    handle_t second = asset_store_load_async(ASYNC_NAME, NULL, HANDLE_NONE);
    shell_systems_poll(0);
    asset_store_cancel(first);
    poll_complete(second);
    TEST_ASSERT_EQUAL_INT(PENDING_READY, asset_store_state(second));
    const asset_pack_t* blocking = asset_store_load(ASYNC_NAME);
    asset_store_cancel(second);
    TEST_ASSERT_EQUAL_PTR(blocking, asset_store_loaded(second));
    asset_store_release(ASYNC_NAME);
    TEST_ASSERT_EQUAL_INT(before + 1, asset_store_mounted());
    TEST_ASSERT_EQUAL_STRING("retained", note_of(blocking));
    asset_store_release(ASYNC_NAME);
    TEST_ASSERT_NOT_NULL(asset_store_load(ASYNC_NAME));
    handle_t failed = asset_store_load_async("", NULL, HANDLE_NONE);
    asset_store_cancel(failed);
    TEST_ASSERT_EQUAL_INT(PENDING_CANCELLED, asset_store_state(failed));
    TEST_ASSERT_EQUAL_INT(before + 1, asset_store_mounted());
    asset_store_release(ASYNC_NAME);
    TEST_ASSERT_EQUAL_INT(before, asset_store_mounted());
    remove_pack(ASYNC_NAME);
}

static void
test_blocking_load_ignores_full_request_pool_and_preserves_generations(void) {
    test_asset_dir_use(".");
    write_pack(ASYNC_NAME, "blocking", 0, NOTE_BYTES);
    handle_t* loads = fill_request_pool("not_opened");
    const int before = asset_store_mounted();
    failure_logs = 0;
    TEST_ASSERT_NOT_NULL(asset_store_load(ASYNC_NAME));
    TEST_ASSERT_EQUAL_UINT(0, failure_logs);
    TEST_ASSERT_EQUAL_INT(before + 1, asset_store_mounted());
    for (uint32_t i = 0; i < ASSET_STORE_REQUESTS_MAX; i++) {
        TEST_ASSERT_EQUAL_INT(PENDING_WAIT, asset_store_state(loads[i]));
        asset_store_cancel(loads[i]);
    }
    asset_store_release(ASYNC_NAME);
    free(loads);
    remove_pack(ASYNC_NAME);
}

static void
test_blocking_finishes_a_shared_partial_mount_without_delivering_callbacks(void) {
    test_asset_dir_use(".");
    write_pack(ASYNC_NAME, "shared", 0, ASYNC_PAYLOAD_BYTES);
    const int before = asset_store_mounted();
    completions = 0;
    failure_logs = 0;
    handle_t load = asset_store_load_async(ASYNC_NAME, loaded_done, HANDLE_NONE);
    shell_systems_poll(0);
    TEST_ASSERT_EQUAL_INT(PENDING_WAIT, asset_store_state(load));
    const asset_pack_t* pack = asset_store_load(ASYNC_NAME);
    TEST_ASSERT_NOT_NULL(pack);
    TEST_ASSERT_EQUAL_INT(0, completions);
    shell_systems_poll(0);
    TEST_ASSERT_EQUAL_PTR(pack, asset_store_loaded(load));
    TEST_ASSERT_EQUAL_INT(1, completions);
    TEST_ASSERT_EQUAL_UINT(0, failure_logs);
    TEST_ASSERT_EQUAL_INT(before + 1, asset_store_mounted());
    asset_store_release(ASYNC_NAME);
    TEST_ASSERT_EQUAL_INT(before + 1, asset_store_mounted());
    asset_store_release(ASYNC_NAME);
    TEST_ASSERT_EQUAL_INT(before, asset_store_mounted());
    remove_pack(ASYNC_NAME);
}

static void
test_failure_logs_and_blocking_async_parity(void) {
    test_asset_dir_use(".");
    const int before = asset_store_mounted();
    char too_long[ASSET_NAME_MAX + 1];
    memset(too_long, 'n', sizeof too_long - 1);
    too_long[sizeof too_long - 1] = '\0';
    const char* names[] = {"suite_missing", "", too_long, "suite_bad_crc", "suite_bad_entry"};
    write_pack("suite_bad_crc", "bad", 1, NOTE_BYTES);

    enum { ENTRY_ALIGNMENT_OFFSET = 44, CRC_OFFSET = 16 };

    const uint32_t capacity = ASSET_PACK_HEADER_SIZE + ASSET_PACK_ENTRY_SIZE + NOTE_BYTES;
    uint8_t* bytes = malloc(capacity);
    TEST_ASSERT_NOT_NULL(bytes);
    test_pack_t bad = test_pack_begin(bytes, capacity, 1);
    (void)test_pack_add(&bad, "note", NOTE_TYPE, NOTE_BYTES);
    const uint32_t size = test_pack_finish(&bad);
    test_pack_put32(bytes + ASSET_PACK_HEADER_SIZE + ENTRY_ALIGNMENT_OFFSET, 0);
    test_pack_put32(bytes + CRC_OFFSET, crc32_update(0, bytes + ASSET_PACK_HEADER_SIZE, size - ASSET_PACK_HEADER_SIZE));
    test_write_file("suite_bad_entry.apak", bytes, size);
    free(bytes);
    for (uint32_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        failure_logs = 0;
        TEST_ASSERT_NULL(asset_store_load(names[i]));
        TEST_ASSERT_EQUAL_UINT(1, failure_logs);
        TEST_ASSERT_EQUAL_INT(before, asset_store_mounted());
        failure_logs = 0;
        handle_t load = asset_store_load_async(names[i], NULL, HANDLE_NONE);
        poll_complete(load);
        TEST_ASSERT_EQUAL_INT(PENDING_FAILED, asset_store_state(load));
        TEST_ASSERT_NULL(asset_store_loaded(load));
        TEST_ASSERT_EQUAL_UINT(1, failure_logs);
        TEST_ASSERT_EQUAL_INT(before, asset_store_mounted());
        asset_store_cancel(load);
    }
    failure_logs = 0;
    handle_t first = asset_store_load_async("suite_bad_crc", NULL, HANDLE_NONE);
    handle_t second = asset_store_load_async("suite_bad_crc", NULL, HANDLE_NONE);
    poll_complete(first);
    TEST_ASSERT_EQUAL_INT(PENDING_FAILED, asset_store_state(second));
    TEST_ASSERT_EQUAL_UINT(1, failure_logs);
    TEST_ASSERT_EQUAL_INT(before, asset_store_mounted());
    asset_store_cancel(first);
    asset_store_cancel(second);
    remove_pack("suite_bad_crc");
    remove_pack("suite_bad_entry");
}

static void
test_dead_tokens_silence_wait_ready_failed_and_preserve_owner_on_delivery(void) {
    test_asset_dir_use(".");
    write_pack(ASYNC_NAME, "tokens", 0, NOTE_BYTES);
    completions = 0;
    const asset_pack_t* blocking = asset_store_load(ASYNC_NAME);
    const char* names[] = {"not_mounted", ASYNC_NAME, ""};
    for (uint32_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        handle_t token = pending_token_new();
        handle_t load = asset_store_load_async(names[i], loaded_done, token);
        pending_token_cancel(token);
        shell_systems_poll(0);
        TEST_ASSERT_EQUAL_INT(0, completions);
        if (i == 1) {
            TEST_ASSERT_EQUAL_INT(PENDING_READY, asset_store_state(load));
            asset_store_release(ASYNC_NAME);
        } else {
            TEST_ASSERT_EQUAL_INT(PENDING_CANCELLED, asset_store_state(load));
        }
        load = asset_store_load_async("not_mounted", loaded_done, token);
        TEST_ASSERT_EQUAL_INT(PENDING_CANCELLED, asset_store_state(load));
    }
    handle_t token = pending_token_new();
    handle_t load = asset_store_load_async(ASYNC_NAME, loaded_done, token);
    shell_systems_poll(0);
    TEST_ASSERT_EQUAL_INT(1, completions);
    TEST_ASSERT_TRUE(handle_equal(token, completed_owner));
    TEST_ASSERT_TRUE(handle_equal(load, completed_request));
    TEST_ASSERT_EQUAL_PTR(blocking, asset_store_loaded(load));
    asset_store_release(ASYNC_NAME);
    asset_store_release(ASYNC_NAME);
    pending_token_cancel(token);
    remove_pack(ASYNC_NAME);
}

static void
test_app_exit_cleans_all_async_states_and_tokens_but_keeps_blocking_use(void) {
    test_asset_dir_use(".");
    write_pack(ASYNC_NAME, "exit", 0, NOTE_BYTES);
    const int before = asset_store_mounted();
    const asset_pack_t* pack = asset_store_load(ASYNC_NAME);
    handle_t token = pending_token_new();
    handle_t ready = asset_store_load_async(ASYNC_NAME, NULL, HANDLE_NONE);
    handle_t grouped = asset_store_load_async(ASYNC_NAME, NULL, token);
    handle_t failed = asset_store_load_async("", NULL, HANDLE_NONE);
    handle_t waiting = asset_store_load_async("not_mounted", NULL, HANDLE_NONE);
    shell_systems_app_exit();
    TEST_ASSERT_FALSE(pending_owner_alive(token));
    const handle_t loads[] = {ready, grouped, failed, waiting};
    for (uint32_t i = 0; i < sizeof loads / sizeof loads[0]; i++) {
        TEST_ASSERT_EQUAL_INT(PENDING_CANCELLED, asset_store_state(loads[i]));
    }
    TEST_ASSERT_EQUAL_INT(before + 1, asset_store_mounted());
    TEST_ASSERT_EQUAL_STRING("exit", note_of(pack));
    asset_store_release(ASYNC_NAME);
    remove_pack(ASYNC_NAME);
}

static void
test_pending_callback_failed_ready_and_wait_are_not_reclaimed(void) {
    test_asset_dir_use(".");
    write_pack(ASYNC_NAME, "pool", 0, NOTE_BYTES);
    TEST_ASSERT_NOT_NULL(asset_store_load(ASYNC_NAME));
    handle_t* loads = malloc(ASSET_STORE_REQUESTS_MAX * sizeof(*loads));
    TEST_ASSERT_NOT_NULL(loads);
    loads[0] = asset_store_load_async("", loaded_done, HANDLE_NONE);
    loads[1] = asset_store_load_async(ASYNC_NAME, NULL, HANDLE_NONE);
    for (uint32_t i = 2; i < ASSET_STORE_REQUESTS_MAX; i++) {
        loads[i] = asset_store_load_async("not_mounted", NULL, HANDLE_NONE);
    }
    TEST_ASSERT_TRUE(handle_is_none(asset_store_load_async("", NULL, HANDLE_NONE)));
    TEST_ASSERT_EQUAL_INT(PENDING_FAILED, asset_store_state(loads[0]));
    TEST_ASSERT_EQUAL_INT(PENDING_READY, asset_store_state(loads[1]));
    for (uint32_t i = 2; i < ASSET_STORE_REQUESTS_MAX; i++) {
        TEST_ASSERT_EQUAL_INT(PENDING_WAIT, asset_store_state(loads[i]));
    }
    shell_systems_app_exit();
    asset_store_release(ASYNC_NAME);
    free(loads);
    remove_pack(ASYNC_NAME);
}

static void
test_reclaim_and_release_use_age_instead_of_slot_order(void) {
    test_asset_dir_use(".");
    write_pack(ASYNC_NAME, "age", 0, NOTE_BYTES);
    handle_t* loads = fill_request_pool("not_mounted");
    asset_store_cancel(loads[1]);
    handle_t oldest = asset_store_load_async("", NULL, HANDLE_NONE);
    asset_store_cancel(loads[0]);
    handle_t newest = asset_store_load_async("", NULL, HANDLE_NONE);
    TEST_ASSERT_TRUE(handle_slot(oldest) > handle_slot(newest));
    handle_t next = asset_store_load_async("", NULL, HANDLE_NONE);
    TEST_ASSERT_EQUAL_INT(PENDING_NONE, asset_store_state(oldest));
    TEST_ASSERT_EQUAL_INT(PENDING_FAILED, asset_store_state(newest));
    asset_store_cancel(next);
    asset_store_cancel(newest);
    for (uint32_t i = 2; i < ASSET_STORE_REQUESTS_MAX; i++) {
        asset_store_cancel(loads[i]);
    }
    TEST_ASSERT_NOT_NULL(asset_store_load(ASYNC_NAME));
    handle_t low = asset_store_load_async(ASYNC_NAME, NULL, HANDLE_NONE);
    oldest = asset_store_load_async(ASYNC_NAME, NULL, HANDLE_NONE);
    asset_store_release(ASYNC_NAME);
    TEST_ASSERT_EQUAL_INT(PENDING_NONE, asset_store_state(low));
    newest = asset_store_load_async(ASYNC_NAME, NULL, HANDLE_NONE);
    TEST_ASSERT_TRUE(handle_slot(oldest) > handle_slot(newest));
    handle_t other = asset_store_load_async("another_name", NULL, HANDLE_NONE);
    asset_store_release(ASYNC_NAME);
    TEST_ASSERT_EQUAL_INT(PENDING_NONE, asset_store_state(oldest));
    TEST_ASSERT_EQUAL_INT(PENDING_READY, asset_store_state(newest));
    TEST_ASSERT_EQUAL_INT(PENDING_WAIT, asset_store_state(other));
    asset_store_release(ASYNC_NAME);
    TEST_ASSERT_EQUAL_INT(PENDING_NONE, asset_store_state(newest));
    asset_store_release(ASYNC_NAME);
    TEST_ASSERT_EQUAL_INT(PENDING_WAIT, asset_store_state(other));
    asset_store_cancel(other);
    free(loads);
    remove_pack(ASYNC_NAME);
}

static void
test_handle_invalid_fields_and_kinds_are_dead_and_cancel_is_noop(void) {
    const handle_t valid = handle_make(HANDLE_OWNER_TOKEN, 7, 29);
    TEST_ASSERT_EQUAL_UINT(HANDLE_OWNER_TOKEN, handle_owner(valid));
    TEST_ASSERT_EQUAL_UINT(7, handle_slot(valid));
    TEST_ASSERT_EQUAL_UINT(29, handle_generation(valid));
    const handle_t invalid[] = {
        handle_make((handle_owner_t)0, 0, 1),
        handle_make((handle_owner_t)(HANDLE_OWNER_MAX + 1U), 0, 1),
        handle_make(HANDLE_OWNER_TOKEN, HANDLE_SLOT_MAX + 1U, 1),
        handle_make(HANDLE_OWNER_TOKEN, 0, 0),
        handle_make(HANDLE_OWNER_TOKEN, 0, HANDLE_GENERATION_MAX + 1U),
    };
    for (uint32_t i = 0; i < sizeof invalid / sizeof invalid[0]; i++) {
        TEST_ASSERT_TRUE(handle_is_none(invalid[i]));
    }
    handle_t load = asset_store_load_async("not_mounted", NULL, HANDLE_NONE);
    const handle_t wrong[] = {
        handle_make(HANDLE_OWNER_TOKEN, handle_slot(load), handle_generation(load)),
        handle_make(HANDLE_OWNER_ASSET_LOAD, ASSET_STORE_REQUESTS_MAX, 1),
        handle_make(HANDLE_OWNER_ASSET_LOAD, handle_slot(load), 0),
    };
    for (uint32_t i = 0; i < sizeof wrong / sizeof wrong[0]; i++) {
        TEST_ASSERT_EQUAL_INT(PENDING_NONE, asset_store_state(wrong[i]));
        asset_store_cancel(wrong[i]);
        TEST_ASSERT_EQUAL_INT(PENDING_WAIT, asset_store_state(load));
        pending_token_cancel(wrong[i]);
    }
    handle_t token = pending_token_new();
    const handle_t bad_tokens[] = {
        handle_make(HANDLE_OWNER_ASSET_LOAD, handle_slot(token), handle_generation(token)),
        handle_make(HANDLE_OWNER_TOKEN, PENDING_TOKENS_MAX, 1),
        (handle_t){.id = ((uint32_t)HANDLE_OWNER_TOKEN << HANDLE_OWNER_SHIFT)
                         | (handle_slot(token) << HANDLE_SLOT_SHIFT)},
    };
    for (uint32_t i = 0; i < sizeof bad_tokens / sizeof bad_tokens[0]; i++) {
        TEST_ASSERT_FALSE(pending_owner_alive(bad_tokens[i]));
        pending_token_cancel(bad_tokens[i]);
        TEST_ASSERT_TRUE(pending_owner_alive(token));
    }
    handle_t child = asset_store_load_async("not_mounted", NULL, load);
    TEST_ASSERT_EQUAL_INT(PENDING_CANCELLED, asset_store_state(child));
    pending_token_cancel(token);
    TEST_ASSERT_FALSE(pending_owner_alive(load));
    asset_store_cancel(load);
}

static handle_t sibling_token, sibling_request;

static void
cancel_sibling_done(handle_t request, pending_t state, handle_t owner) {
    loaded_done(request, state, owner);
    pending_token_cancel(sibling_token);
}

static void
test_callback_can_cancel_later_sibling_before_delivery(void) {
    test_asset_dir_use(".");
    write_pack(ASYNC_NAME, "sibling", 0, NOTE_BYTES);
    completions = 0;
    sibling_token = pending_token_new();
    handle_t first = asset_store_load_async(ASYNC_NAME, cancel_sibling_done, HANDLE_NONE);
    sibling_request = asset_store_load_async(ASYNC_NAME, loaded_done, sibling_token);
    shell_systems_poll(0);
    TEST_ASSERT_EQUAL_INT(1, completions);
    TEST_ASSERT_EQUAL_INT(PENDING_READY, asset_store_state(first));
    TEST_ASSERT_EQUAL_INT(PENDING_CANCELLED, asset_store_state(sibling_request));
    asset_store_release(ASYNC_NAME);
    remove_pack(ASYNC_NAME);
}

static void
test_callback_request_for_unmounted_pack_starts_next_pass(void) {
    test_asset_dir_use(".");
    write_pack("suite_async", "child", 0, ASYNC_PAYLOAD_BYTES);
    write_pack(ASYNC_NAME, "parent", 0, NOTE_BYTES);
    completions = 0;
    handle_t parent = asset_store_load_async(ASYNC_NAME, spawn_done, HANDLE_NONE);
    shell_systems_poll(0);
    TEST_ASSERT_EQUAL_INT(PENDING_READY, asset_store_state(parent));
    TEST_ASSERT_EQUAL_INT(PENDING_WAIT, asset_store_state(callback_child));
    TEST_ASSERT_NULL(asset_store_request_find(callback_child)->mount);
    TEST_ASSERT_EQUAL_INT(1, completions);
    poll_complete(callback_child);
    TEST_ASSERT_EQUAL_INT(2, completions);
    asset_store_release(ASYNC_NAME);
    asset_store_release("suite_async");
    remove_pack(ASYNC_NAME);
    remove_pack("suite_async");
}

static void
test_tokens_exhaust_to_a_dead_owner_and_exit_restores_slots(void) {
    handle_t* tokens = malloc(PENDING_TOKENS_MAX * sizeof(*tokens));
    TEST_ASSERT_NOT_NULL(tokens);
    uint32_t count = 0;
    handle_t token;
    do {
        token = pending_token_new();
        if (pending_owner_alive(token)) {
            TEST_ASSERT_LESS_THAN_UINT(PENDING_TOKENS_MAX, count);
            tokens[count++] = token;
        }
    } while (pending_owner_alive(token));
    TEST_ASSERT_FALSE(handle_is_none(token));
    TEST_ASSERT_TRUE(handle_equal(PENDING_OWNER_DEAD, token));
    handle_t load = asset_store_load_async("not_mounted", loaded_done, token);
    TEST_ASSERT_EQUAL_INT(PENDING_CANCELLED, asset_store_state(load));
    shell_systems_app_exit();
    for (uint32_t i = 0; i < count; i++) {
        TEST_ASSERT_FALSE(pending_owner_alive(tokens[i]));
    }
    token = pending_token_new();
    TEST_ASSERT_TRUE(pending_owner_alive(token));
    pending_token_cancel(token);
    free(tokens);
}

static void
test_request_generation_wrap_retires_all_slots_without_reviving_stale_handles(void) {
    asset_store_request_t* saved = malloc(ASSET_STORE_REQUESTS_MAX * sizeof(*saved));
    TEST_ASSERT_NOT_NULL(saved);
    for (uint32_t i = 0; i < ASSET_STORE_REQUESTS_MAX; i++) {
        saved[i] = *asset_store_request_at(i);
        *asset_store_request_at(i) = (asset_store_request_t){.generation = HANDLE_GENERATION_MAX - 1U};
    }
    handle_t stale = HANDLE_NONE;
    for (uint32_t i = 0; i < ASSET_STORE_REQUESTS_MAX; i++) {
        handle_t load = asset_store_load_async("not_mounted", NULL, HANDLE_NONE);
        TEST_ASSERT_FALSE(handle_is_none(load));
        TEST_ASSERT_EQUAL_UINT(i, handle_slot(load));
        TEST_ASSERT_EQUAL_UINT(HANDLE_GENERATION_MAX, handle_generation(load));
        if (i == 0) {
            stale = load;
        }
        asset_store_cancel(load);
    }
    TEST_ASSERT_TRUE(handle_is_none(asset_store_load_async("not_mounted", NULL, HANDLE_NONE)));
    TEST_ASSERT_EQUAL_INT(PENDING_CANCELLED, asset_store_state(stale));
    for (uint32_t i = 0; i < ASSET_STORE_REQUESTS_MAX; i++) {
        *asset_store_request_at(i) = saved[i];
    }
    free(saved);
}

static void
test_asset_system_registers_poll_and_exit_cleanup(void) {
    shell_system_t* real = shell_system_swap_for_test(NULL);
    shell_system_swap_for_test(real);
    const shell_system_t* asset = real;
    while (asset != NULL && strcmp(asset->name, "asset") != 0) {
        asset = asset->next;
    }
    TEST_ASSERT_NOT_NULL(asset);
    TEST_ASSERT_EQUAL_INT(SHELL_ORDER_ASSET, asset->order);
    TEST_ASSERT_NOT_NULL(asset->poll);
    TEST_ASSERT_EQUAL_PTR(asset_store_app_exit, asset->app_exit);
}

static void
test_step_budget_is_shared_by_distinct_pending_mounts(void) {
    test_asset_dir_use(".");
    write_pack(ASYNC_NAME, "first", 0, ASYNC_PAYLOAD_BYTES);
    write_pack("suite_async_other", "second", 0, ASYNC_PAYLOAD_BYTES);
    handle_t first = asset_store_load_async(ASYNC_NAME, NULL, HANDLE_NONE);
    handle_t second = asset_store_load_async("suite_async_other", NULL, HANDLE_NONE);
    for (uint32_t i = 0; i < ASYNC_STEPS; i++) {
        shell_systems_poll(0);
    }
    TEST_ASSERT_EQUAL_INT(PENDING_READY, asset_store_state(first));
    TEST_ASSERT_EQUAL_INT(PENDING_WAIT, asset_store_state(second));
    poll_complete(second);
    TEST_ASSERT_EQUAL_INT(PENDING_READY, asset_store_state(second));
    asset_store_release(ASYNC_NAME);
    asset_store_release("suite_async_other");
    remove_pack(ASYNC_NAME);
    remove_pack("suite_async_other");
}

#endif

void
suite_asset_store(void) {
    RUN_TEST(test_a_directory_opens_and_finds_each_pack_by_name);
    RUN_TEST(test_a_bad_magic_version_or_checksum_is_refused);
    RUN_TEST(test_a_row_out_of_range_misaligned_or_overlapping_is_refused);
    RUN_TEST(test_two_packs_with_one_name_or_a_name_with_no_room_for_its_nul_are_refused);
#ifndef DEVICE_BUILD
    RUN_TEST(test_cancel_ready_retains_every_use_and_dead_owner_does_too);
    RUN_TEST(test_cancel_wait_failed_and_blocking_holder_preserve_other_uses);
    RUN_TEST(test_blocking_load_ignores_full_request_pool_and_preserves_generations);
    RUN_TEST(test_blocking_finishes_a_shared_partial_mount_without_delivering_callbacks);
    RUN_TEST(test_failure_logs_and_blocking_async_parity);
    RUN_TEST(test_dead_tokens_silence_wait_ready_failed_and_preserve_owner_on_delivery);
    RUN_TEST(test_app_exit_cleans_all_async_states_and_tokens_but_keeps_blocking_use);
    RUN_TEST(test_pending_callback_failed_ready_and_wait_are_not_reclaimed);
    RUN_TEST(test_reclaim_and_release_use_age_instead_of_slot_order);
    RUN_TEST(test_handle_invalid_fields_and_kinds_are_dead_and_cancel_is_noop);
    RUN_TEST(test_callback_can_cancel_later_sibling_before_delivery);
    RUN_TEST(test_callback_request_for_unmounted_pack_starts_next_pass);
    RUN_TEST(test_tokens_exhaust_to_a_dead_owner_and_exit_restores_slots);
    RUN_TEST(test_request_generation_wrap_retires_all_slots_without_reviving_stale_handles);
    RUN_TEST(test_asset_system_registers_poll_and_exit_cleanup);
    RUN_TEST(test_step_budget_is_shared_by_distinct_pending_mounts);
    RUN_TEST(test_async_known_size_takes_bounded_steps_and_shares_one_mount);
    RUN_TEST(test_cancel_frees_partial_mount_and_reuses_a_generation);
    RUN_TEST(test_token_cancel_silences_partial_and_already_ready_callbacks);
    RUN_TEST(test_failure_is_pollable_once_delivered_and_oldest_is_reclaimed);
    RUN_TEST(test_async_example_releases_or_cancels_its_visit);
    RUN_TEST(test_token_generation_wrap_retires_the_slot);
    RUN_TEST(test_callback_requests_wait_a_pass_and_blocking_load_never_delivers_them);
    RUN_TEST(test_request_pool_limits_pending_work_and_cancel_returns_a_slot);
    RUN_TEST(test_handle_fields_and_generation_retirement);
    RUN_TEST(test_async_delivery_is_deferred_and_once);
    RUN_TEST(test_the_load_pack_example_reads_an_entry_and_releases_the_pack);
    RUN_TEST(test_two_packs_mount_alone_and_a_bad_checksum_in_one_leaves_the_other_usable);
    RUN_TEST(test_a_pack_is_read_on_its_first_use_and_again_only_after_its_last_release);
    RUN_TEST(test_a_mount_past_the_store_s_slots_is_refused_until_one_is_released);
#endif
}

SUITE_REGISTER(suite_asset_store);
