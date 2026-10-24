/* host_main: Unity lifecycle for standalone package suites. */
#include "registry.h"

#include <string.h>

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

int
main(int argc, char** argv) {
    if (argc == 2 && strcmp(argv[1], TESTKIT_LIST_OPTION) == 0) {
        return testkit_run(argc, argv);
    }
    UNITY_BEGIN();
    int errors = testkit_run(argc, argv);
    return UNITY_END() + errors;
}
