/* host_main: standalone selection and Unity lifecycle for package suites. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "testkit/suite.h"
#include "unity.h"

#define TESTKIT_LIST_OPTION       "--list"
#define TESTKIT_ON_REQUEST_OPTION "--on-request"

static void
list_suites(void) {
    for (size_t i = 0; i < suites_count(); i++) {
        puts(suites_at(i)->name);
    }
}

static int
run_named(int argc, char** argv) {
    int errors = 0;
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
            (void)fprintf(stderr, "Unknown suite: %s\n", argv[arg]);
            errors++;
        }
    }
    return errors;
}

int
main(int argc, char** argv) {
    int errors = 0;
    if (argc == 2 && strcmp(argv[1], TESTKIT_LIST_OPTION) == 0) {
        list_suites();
        return suites_dropped() != 0 || suites_count() == 0 ? EXIT_FAILURE : EXIT_SUCCESS;
    }
    UNITY_BEGIN();
    if (argc == 1) {
        errors += suites_run(false);
    } else if (argc == 2 && strcmp(argv[1], TESTKIT_ON_REQUEST_OPTION) == 0) {
        errors += suites_run(true);
    } else {
        errors += run_named(argc, argv) + suites_dropped() + (suites_count() == 0);
    }
    int failures = UNITY_END();
    return failures != 0 || errors != 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
