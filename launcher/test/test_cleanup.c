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
    if (test_cleanup != NULL) {
        test_cleanup();
        suite_clear_test_cleanup();
    }
}
