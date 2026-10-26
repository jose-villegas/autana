/* Deferred cleanups registered by test fixtures. */
#include "testkit/cleanup.h"

#include <setjmp.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

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
    /* Each is dropped before it runs, so one that fails its own assertion
     * runs once. The rest still run; the failure is raised once they have. */
    jmp_buf outer;
    memcpy(outer, Unity.AbortFrame, sizeof outer);
    volatile bool failed = false;
    while (cleanup_count > 0) {
        void (*const cleanup)(void) = cleanups[--cleanup_count];
        if (TEST_PROTECT()) {
            cleanup();
        } else {
            failed = true;
        }
    }
    memcpy(Unity.AbortFrame, outer, sizeof outer);
    if (failed) {
        TEST_ABORT();
    }
}
