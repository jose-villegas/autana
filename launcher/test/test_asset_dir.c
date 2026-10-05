#define _POSIX_C_SOURCE 200809L /* setenv, unsetenv, strdup */

#include "test_asset_dir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"

#define DIR_ENV "AUTANA_ASSET_DIR"

static char* saved;

static void
set_dir(const char* dir) {
#ifdef _WIN32
    TEST_ASSERT_EQUAL_INT(0, _putenv_s(DIR_ENV, dir == NULL ? "" : dir));
#else
    TEST_ASSERT_EQUAL_INT(0, dir == NULL ? unsetenv(DIR_ENV) : setenv(DIR_ENV, dir, 1));
#endif
}

void
test_asset_dir_use(const char* dir) {
    const char* runner = getenv(DIR_ENV);
    free(saved);
    saved = runner == NULL ? NULL : strdup(runner);
    set_dir(dir);
}

void
test_asset_dir_restore(void) {
    set_dir(saved);
    free(saved);
    saved = NULL;
}

void
test_write_file(const char* path, const void* bytes, size_t size) {
    FILE* file = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL_MESSAGE(file, path);
    TEST_ASSERT_EQUAL_size_t(size, fwrite(bytes, 1, size, file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
}
