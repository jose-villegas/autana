/* host_main: standalone selection and Unity lifecycle for package suites. */
#include <stdio.h>
#include <string.h>

#include "testkit/cleanup.h"
#include "testkit/suite.h"
#include "unity.h"

#define TESTKIT_LIST_OPTION "--list"

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
    int errors = suites_dropped();
    if (argc == 2 && strcmp(argv[1], TESTKIT_LIST_OPTION) == 0) {
        for (size_t i = 0; i < suites_count(); i++) {
            puts(suites_at(i)->name);
        }
        return errors;
    }
    UNITY_BEGIN();
    if (argc == 1) {
        for (size_t i = 0; i < suites_count(); i++) {
            const suite_entry_t* suite = suites_at(i);
            if (!suite->on_request) {
                suite->fn();
            }
        }
    }
    for (int arg = 1; arg < argc; arg++) {
        const suite_entry_t* suite = NULL;
        for (size_t i = 0; i < suites_count(); i++) {
            if (strcmp(argv[arg], suites_at(i)->name) == 0) {
                suite = suites_at(i);
                break;
            }
        }
        if (suite != NULL) {
            suite->fn();
        } else {
            fprintf(stderr, "Unknown suite: %s\n", argv[arg]);
            errors++;
        }
    }
    return UNITY_END() + errors;
}
