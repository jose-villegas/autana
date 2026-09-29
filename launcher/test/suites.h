/*
 * The test suites, shared by both runners.
 *
 * Every suite here is compiled into BOTH:
 *   - the host runner (test/host_main.c), for a sub-second TDD loop
 *   - the shipped firmware (main/selftest.c), which runs them at boot
 *
 * Running the same suites in both places is deliberate. The host loop is for
 * developing; the on-device run proves the code behaves identically when built
 * by the RISC-V toolchain and executed on the real chip, which is not something
 * a laptop can vouch for.
 *
 * Suites REGISTER THEMSELVES with SUITE_REGISTER, so there is no list to keep
 * in step. That matters most for app-owned suites: a suite living in
 * main/apps/<name>/ disappears with its app when the folder is deleted, and
 * nothing else needs editing. See main/app.h for the same pattern applied to
 * apps themselves.
 *
 * A portable suite must not include any ESP-IDF or hardware header, so it can
 * link on a host. Suites that need the chip are guarded with DEVICE_BUILD and
 * are simply not compiled into the host runner.
 */
#pragma once

#include <stdbool.h>

/* Headroom over the ~130 suites registered; a suite past it fails the run. */
#define SUITE_MAX 192

typedef void (*suite_fn)(void);

/* Called by the SUITE_REGISTER macros before main(). `on_request` keeps a
 * suite out of suites_run_all() while leaving suites_run_request() able to find
 * it: a sweep measured in hours belongs to whoever asks for it by name, not
 * to every boot of every image that carries it. */
void suite_register(const char* name, suite_fn fn);
void suite_register_on_request(const char* name, suite_fn fn);

#define SUITE_REGISTER(fn)                                                                                             \
    __attribute__((constructor)) static void fn##_register(void) { suite_register(#fn, fn); }

#define SUITE_REGISTER_ON_REQUEST(fn)                                                                                  \
    __attribute__((constructor)) static void fn##_register(void) { suite_register_on_request(#fn, fn); }

/* Runs every registered suite, in name order so the output is stable. */
void suites_run_all(void);

/* One RUNSUITE request: "<suite>", or "<suite> <pattern>[,<pattern>...]" to
 * run only the tests whose name contains a pattern (case-sensitive substring).
 * A pattern holds at most SUITE_FILTER_LEN - 1 characters and a request at
 * most SUITE_FILTER_MAX of them; one past either is refused, nothing runs.
 * While filtering, suites_test_runs() prints "SUITE_TEST name=... selected=..."
 * for every test reached. The patterns live for the call only. */
#define SUITE_NAME_MAX   38
#define SUITE_FILTER_MAX 8
#define SUITE_FILTER_LEN 40

typedef struct {
    char name[SUITE_NAME_MAX + 1];
    bool found;    /* a suite of that exact name is registered */
    bool refused;  /* a pattern was too long, empty, or one too many */
    int selected;  /* tests that ran */
    int unmatched; /* patterns that matched no test */
} suite_run_t;

suite_run_t suites_run_request(const char* request);

/* The completion line a harness waits for - one owner, so the tools that parse
 * it are pinned to what this prints. */
void suites_print_run(const suite_run_t* run);

/* Asked by suite_run_test_timed() (timing.c) for each test: true to run it. */
bool suites_test_runs(const char* test_name);

/* How many suites did NOT fit and were dropped - see suite_register().
 *
 * Both runners fail when this is nonzero. It is checked there rather than in
 * a suite of its own for the obvious reason: a guard that registers like
 * everything else could be the very suite that gets dropped, and would then
 * be the one thing not reporting the problem. */
int suites_dropped(void);

/* Prints how many tests since the last call presented past the frame
 * watch's warm-up (util/frame_watch.h), so a run shows how much of it was
 * judged. Defined beside the RUN_TEST wrapper, in timing.c. */
void suite_report_frame_watch(void);
