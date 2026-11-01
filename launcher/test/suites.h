/*
 * The test suites, shared by both runners.
 *
 * Every suite here is compiled into BOTH:
 *   - the host runner (test/host_main.c), for a sub-second TDD loop
 *   - the shipped firmware (main/selftest/selftest.c), which runs them at boot
 *
 * Running the same suites in both places is deliberate. The host loop is for
 * developing; the on-device run proves the code behaves identically when built
 * by the RISC-V toolchain and executed on the real chip, which is not something
 * a laptop can vouch for.
 *
 * Suites REGISTER THEMSELVES with SUITE_REGISTER, so there is no list to keep
 * in step. That matters most for app-owned suites: a suite living in
 * main/apps/<name>/ disappears with its app when the folder is deleted, and
 * nothing else needs editing. See main/app/app.h for the same pattern applied to
 * apps themselves.
 *
 * A portable suite must not include any ESP-IDF or hardware header, so it can
 * link on a host. Suites that need the chip are guarded with DEVICE_BUILD and
 * are simply not compiled into the host runner.
 */
#pragma once

#include "testkit/suite.h"

#include <stdbool.h>
#include <stddef.h>

/* SUITE_READS(fn, "pack", ...) names the packs suite `fn` reads from the
 * runner's asset folder (asset/asset_store.h), so a run whose pack waits on the
 * bake lock skips that suite rather than fail it. The host runner fails a run
 * in which a suite reads a pack it did not name, so no read goes unnamed.
 * At most SUITE_READS_MAX suites name packs; one past it fails the run. */
#define SUITE_READS_MAX 16
void suite_reads(const char* name, const char* const* packs);

#define SUITE_READS(fn, ...)                                                                                           \
    __attribute__((constructor)) static void fn##_reads(void) {                                                        \
        static const char* const packs[] = {__VA_ARGS__, NULL};                                                        \
        suite_reads(#fn, packs);                                                                                       \
    }

/* True when the suite running now named `pack` with SUITE_READS. */
bool suites_current_reads(const char* pack);

/* The suite running now, or NULL between suites. */
const char* suites_current(void);

/* Pack `pack` was not built: suites_run_all() skips every suite that reads it,
 * and suites_print_waiting() names them. At most SUITE_WAITING_MAX packs. */
#define SUITE_WAITING_MAX 16
void suites_wait_on(const char* pack);
void suites_print_waiting(void);

/* Calls `each` for every registered suite, in name order. */
typedef void (*suite_each_fn)(const char* name, bool on_request, void* context);
void suites_each(suite_each_fn each, void* context);

/* Runs every registered suite, in name order so the output is stable. */
void suites_run_all(void);

/* One RUNSUITE request: "<suite>", or "<suite> <pattern>[,<pattern>...]" to
 * run only the tests a pattern picks. A pattern holds at most
 * SUITE_FILTER_LEN - 1 characters and a request at most SUITE_FILTER_MAX;
 * one past either is refused. While filtering, suites_test_runs() prints
 * "SUITE_TEST name=... selected=..." for every test reached. */
#define SUITE_FILTER_MAX 4
#define SUITE_FILTER_LEN 32

typedef struct {
    char name[SUITE_NAME_MAX + 1];
    bool found;    /* a suite of that exact name is registered */
    bool refused;  /* a pattern was too long, empty, or one too many */
    int selected;  /* tests that ran */
    int unmatched; /* patterns that matched no test */
} suite_run_t;

void suites_run_request(const char* request, suite_run_t* run);

/* The completion line a harness waits for: one owner, so the tools that parse
 * it are pinned to what this prints. */
void suites_print_run(const suite_run_t* run);

/* Asked by suite_run_test_timed() (timing.c) for each test: true to run it. */
bool suites_test_runs(const char* test_name);

/* Whether `pattern` picks test `name`: each pattern segment equals one of the
 * name's case segments, in order, but the first may instead be part of its
 * function name. An empty segment picks nothing. The tools repeat the rule;
 * both are held to suite_filter_cases.inc. */
#define SUITE_TEST_SEPARATOR '/'
bool suite_test_matches(const char* pattern, const char* name);

/* RUN_TEST_CASE(func, "segment", ...) runs func as RUN_TEST does, named
 * func/segment/..., so each pass of a table loop has a name a pattern can
 * pick. A NULL or empty segment is left out. A name past SUITE_TEST_NAME_MAX,
 * or a segment of other than SUITE_TEST_SEGMENT_CHARS, which the tools'
 * result lines read, is a SUITE OVERFLOW, not run. A case never runs another. */
#define SUITE_TEST_NAME_MAX      95
#define SUITE_TEST_SEGMENT_CHARS "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-"
#define RUN_TEST_CASE(func, ...)                                                                                       \
    suite_run_test_case(func, #func, (const char* const[]){__VA_ARGS__},                                               \
                        sizeof((const char* const[]){__VA_ARGS__}) / sizeof(const char*), __LINE__)
void suite_run_test_case(void (*func)(void), const char* name, const char* const* segments, size_t count, int line);

/* Prints how many tests since the last call presented past the frame
 * watch's warm-up (profile/frame_watch.h), so a run shows how much of it was
 * judged. Defined beside the RUN_TEST wrapper, in timing.c. */
void suite_report_frame_watch(void);
