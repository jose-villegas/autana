/* Deferred cleanup registered by a test fixture. */
#include "test_cleanup.h"

#include <stddef.h>

static void (*test_cleanup)(void);

void
suite_set_test_cleanup(void (*cleanup)(void)) {
    test_cleanup = cleanup;
}

void
suite_clear_test_cleanup(void) {
    test_cleanup = NULL;
}

void
suite_run_test_cleanup(void) {
    /* Cleared first: a cleanup that fails its own assertion runs once. */
    void (*const cleanup)(void) = test_cleanup;
    suite_clear_test_cleanup();
    if (cleanup != NULL) {
        cleanup();
    }
}
