#define _POSIX_C_SOURCE 200809L /* setenv, unsetenv, strdup */

#include "test_asset_dir.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asset/asset_store.h"
#include "test_cleanup.h"
#include "unity.h"

static char* saved;
static bool active;

void
test_asset_dir_set(const char* dir) {
#ifdef _WIN32
    TEST_ASSERT_EQUAL_INT(0, _putenv_s(ASSET_STORE_DIR_ENV, dir == NULL ? "" : dir));
#else
    TEST_ASSERT_EQUAL_INT(0, dir == NULL ? unsetenv(ASSET_STORE_DIR_ENV) : setenv(ASSET_STORE_DIR_ENV, dir, 1));
#endif
}

void
test_asset_dir_restore(void) {
    if (!active) {
        return;
    }
    active = false;
    char* const runner = saved;
    saved = NULL;
    test_asset_dir_set(runner);
    free(runner);
}

void
test_asset_dir_use(const char* dir) {
    if (!active) {
        const char* runner = getenv(ASSET_STORE_DIR_ENV);
        saved = runner == NULL ? NULL : strdup(runner);
        active = true;
        suite_add_test_cleanup(test_asset_dir_restore);
    }
    test_asset_dir_set(dir);
}

void
test_write_file(const char* path, const void* bytes, size_t size) {
    FILE* file = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL_MESSAGE(file, path);
    TEST_ASSERT_EQUAL_size_t(size, fwrite(bytes, 1, size, file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
}
