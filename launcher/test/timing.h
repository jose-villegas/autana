/*
 * Per-test wall time, without editing a single suite file.
 *
 * Every suite calls Unity's RUN_TEST(func) directly. RUN_TEST is a macro
 * defined in unity_internals.h behind "#ifndef RUN_TEST" - if it is already
 * defined by the time that header is processed, Unity quietly steps aside
 * (that is what UNITY_SKIP_DEFAULT_RUNNER exists for). Forcing this header
 * in ahead of a suite's own "#include unity.h" - via the build's -include
 * flag, see main/CMakeLists.txt and test/run_tests.sh - makes that happen
 * for every suite at once, including the ones this project is not free to
 * edit right now and the two dozen others not worth touching just for this.
 *
 * Do NOT force this onto framework/unity.c's own compilation: that trips
 * the same guard from the other side and compiles UnityDefaultTestRun's
 * body out entirely (it becomes "the replacement runner"), which breaks the
 * only function this file's .c half calls. Both build scripts keep it out.
 */
#pragma once

#include "test_cleanup.h"

#define RUN_TEST(func) suite_run_test_timed(func, #func, __LINE__)

/* Runs the test exactly as RUN_TEST always has (same file:line:name:PASS
 * line, byte for byte - see timing.c), then logs a separate line with how
 * long it took. */
void suite_run_test_timed(void (*func)(void), const char* name, int line);

#ifdef DEVICE_BUILD
#include <stdbool.h>
#include <stddef.h>

#include "util/frame_watch.h"

/* What one test body left behind. The body runs a second time when the
 * heap dropped, and leaked_* is what that second run lost. */
typedef struct {
    size_t leaked_8bit;
    size_t leaked_32bit;
    frame_watch_verdict_t watch;
    size_t stack_free;   /* the main task's high-water mark after the first run */
    bool stack_deepened; /* the first run set that mark */
} suite_test_verdict_t;

/* Runs body the way RUN_TEST runs every test on the board and fills in
 * verdict, judging nothing; TEST_PASS() included, however body ends. It
 * leaves Unity's abort frame spent: a caller inside a test restores its own.
 * suite_judge_watched_test() judges RUN_TEST's own, from tearDown(). */
void suite_run_body(void (*body)(void), suite_test_verdict_t* verdict);
void suite_judge_watched_test(void);
#endif
