/* Deferred cleanups registered by test fixtures. */
#include "test_cleanup.h"

#include <stddef.h>

#include "unity.h"

static void (*cleanups[TEST_CLEANUP_MAX])(void);
static size_t cleanup_count;

void
suite_add_test_cleanup(void (*cleanup)(void)) {
    for (size_t i = 0; i < cleanup_count; i++) {
        if (cleanups[i] == cleanup) {
            return;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(cleanup_count < TEST_CLEANUP_MAX, "too many test cleanups: raise TEST_CLEANUP_MAX");
    cleanups[cleanup_count++] = cleanup;
}

void
suite_set_test_cleanup(void (*cleanup)(void)) {
    suite_clear_test_cleanup();
    suite_add_test_cleanup(cleanup);
}

void
suite_clear_test_cleanup(void) {
    cleanup_count = 0;
}

void
suite_run_test_cleanup(void) {
    /* Each is dropped before it runs: a cleanup that fails its own assertion
     * runs once, and the ones still listed run on the next call. */
    while (cleanup_count > 0) {
        cleanups[--cleanup_count]();
    }
}
