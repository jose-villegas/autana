/*
 * Host runner - the fast loop.
 *
 * Builds and runs in well under a second, which is what makes
 * red-green-refactor practical. It runs only the portable suites; the
 * hardware-dependent ones live in the firmware and run on the device.
 *
 * The same suite sources are compiled into the firmware's self-test, so a
 * green run here is the same set of assertions the board will make.
 */

#include <stdio.h>
#include <string.h>

#include "suites.h"
#include "unity.h"

#include "app.h"

/* This process's own path, set below before any suite runs - the duplicate-
 * name test in suite_app_registry.c re-execs it with DUPLICATE_PROBE_ARG to
 * watch app_register()'s assert() actually abort, which is not something a
 * suite can observe happening in its own process. */
const char* g_host_main_path;

#define DUPLICATE_PROBE_ARG "--app-registry-duplicate-probe"

/* Registers two apps under one name and returns cleanly only if
 * app_register()'s assert() failed to catch it - the parent process reads
 * this exiting cleanly as that failure. */
static int
run_duplicate_app_probe(void) {
    static app_t one = {.name = "duplicate-probe"};
    static app_t two = {.name = "duplicate-probe"};
    app_register(&one);
    app_register(&two);
    return 0;
}

/* Unity requires these once per binary. Suites manage their own fixtures,
 * because several of them share this program. */
void
setUp(void) {}

void
tearDown(void) {}

int
main(int argc, char** argv) {
    if (argc > 1 && strcmp(argv[1], DUPLICATE_PROBE_ARG) == 0) {
        return run_duplicate_app_probe();
    }
    g_host_main_path = argv[0];

    UNITY_BEGIN();

    suites_run_all();

    int failures = UNITY_END();

    /* A suite that did not fit is a test that did not run, so this run must
     * not come back green having quietly checked less than the whole set. */
    if (suites_dropped() > 0) {
        printf("FAIL: %d suite(s) dropped; raise SUITE_MAX in suites.h\n", suites_dropped());
        failures += suites_dropped();
    }
    if (suite_leaks() > 0) {
        printf("FAIL: %d test(s) leaked arena blocks\n", suite_leaks());
        failures += suite_leaks();
    }
    return failures;
}
