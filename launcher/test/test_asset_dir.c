#define _POSIX_C_SOURCE 200809L /* setenv, unsetenv, strdup */

#include "test_asset_dir.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asset/asset_store.h"
#include "testkit/cleanup.h"
#include "unity.h"

static char* saved;
static bool active;

static int
put_dir(const char* dir) {
#ifdef _WIN32
    return _putenv_s(ASSET_STORE_DIR_ENV, dir == NULL ? "" : dir);
#else
    return dir == NULL ? unsetenv(ASSET_STORE_DIR_ENV) : setenv(ASSET_STORE_DIR_ENV, dir, 1);
#endif
}

void
test_asset_dir_set(const char* dir) {
    TEST_ASSERT_EQUAL_INT(0, put_dir(dir));
}

void
test_asset_dir_restore(void) {
    if (!active) {
        return;
    }
    active = false;
    char* const runner = saved;
    saved = NULL;
    /* Asserted last: a failed test must not skip the free. */
    const int status = put_dir(runner);
    free(runner);
    TEST_ASSERT_EQUAL_INT(0, status);
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
