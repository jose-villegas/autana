/* lifecycle: standalone Unity fixture cleanup. */
#include "testkit/cleanup.h"
#include "unity.h"

void
setUp(void) {
    suite_clear_test_cleanup();
}

void
tearDown(void) {
    suite_run_test_cleanup();
}
