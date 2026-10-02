/*
 * Host runner: the fast loop.
 *
 * Builds and runs in well under a second, which is what makes
 * red-green-refactor practical. It runs only the portable suites; the
 * hardware-dependent ones live in the firmware and run on the device.
 *
 * The same suite sources are compiled into the firmware's self-test, so a
 * green run here is the same set of assertions the board will make.
 */

#include <signal.h>
#include <stdio.h>
#include <string.h>

#include "heap_arena.h"
#include "suites.h"
#include "unity.h"

/* Unity requires these once per binary. The runner owns the memory audit. */
static size_t heap_blocks_before;
static size_t heap_bytes_before;

void
setUp(void) {
    heap_arena_snapshot(&heap_blocks_before, &heap_bytes_before);
}

void
tearDown(void) {
    suite_run_test_cleanup();

    size_t heap_blocks_after;
    size_t heap_bytes_after;
    heap_arena_snapshot(&heap_blocks_after, &heap_bytes_after);
    if (heap_blocks_after != heap_blocks_before) {
        char message[128];
        (void)snprintf(message, sizeof(message), "test changed arena blocks %zu -> %zu (%zu -> %zu bytes)",
                       heap_blocks_before, heap_blocks_after, heap_bytes_before, heap_bytes_after);
        TEST_FAIL_MESSAGE(message);
    }
}

/* `host_tests --run "<suite> [patterns]"` (repeatable) is a RUNSUITE as the
 * board takes it, for the tooling tests that read its output. */
static void
run_requests(int argc, char** argv) {
    for (int i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], "--run") == 0) {
            const suite_run_t run = suites_run_request(argv[i + 1]);
            suites_print_run(&run);
        }
    }
}

/* A test that corrupts the heap, or trips an assert, ends the process, and
 * Unity's buffered results go with it. Name the test that was running as a
 * failure, in Unity's own format, so the break is red by name rather than a
 * silent crash. */
static void
name_the_test_that_aborted(int signal_number) {
    (void)fflush(stdout);
    (void)printf("%s:%u:%s:FAIL: the process aborted (an assert, or heap_arena's report above)\n",
                 Unity.TestFile == NULL ? "?" : Unity.TestFile, (unsigned)Unity.CurrentTestLineNumber,
                 Unity.CurrentTestName == NULL ? "?" : Unity.CurrentTestName);
    (void)fflush(stdout);
    (void)signal(signal_number, SIG_DFL);
    (void)raise(signal_number);
}

int
main(int argc, char** argv) {
    (void)signal(SIGABRT, name_the_test_that_aborted);
    UNITY_BEGIN();

    if (argc > 1) {
        run_requests(argc, argv);
    } else {
        suites_run_all();
    }

    int failures = UNITY_END();

    /* A suite that did not fit is a test that did not run, so this run must
     * not come back green having quietly checked less than the whole set. */
    if (suites_dropped() > 0) {
        printf("FAIL: %d suite(s) dropped; raise SUITE_MAX in suites.h\n", suites_dropped());
        failures += suites_dropped();
    }
    return failures;
}
