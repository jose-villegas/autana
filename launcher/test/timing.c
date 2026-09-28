/*
 * Implements the RUN_TEST override declared in timing.h.
 *
 * Wraps Unity's own dispatcher (UnityDefaultTestRun) from outside: the timer
 * starts before it and stops after it returns, so nothing here runs inside
 * setUp(), the test body, or tearDown(). That matters because a handful of
 * tests time their own subject with esp_timer_get_time() around a narrower
 * window (one call of the thing under test, say) - this must never be what
 * widens that window.
 *
 * The elapsed-time line is printed AFTER UnityDefaultTestRun returns, so it
 * never touches the existing "file:line:name:PASS" line, which
 * launcher/tools/sweeps/validate_capture.py and an app's own
 * performance-report script already parse.
 */
#include "timing.h"

#include <stdint.h>
#include <stdio.h>

#ifdef DEVICE_BUILD
#include "esp_timer.h"
#else
#include <time.h>
#endif

#ifdef HOST_HEAP_ARENA
#include "heap_arena.h"
#endif

#ifdef DEVICE_BUILD
#include "unity.h"
#include "util/frame_watch.h"
#endif

/* Not pulled from unity.h: that header only declares this when RUN_TEST is
 * NOT already defined (see timing.h's top comment) - the opposite of this
 * file's own situation, since it is what RUN_TEST now expands to. The real
 * definition lives in Unity's own unity.c/UnityDefaultTestRun and is
 * untouched; this is only the prototype, hand-matched to it. */
extern void UnityDefaultTestRun(void (*Func)(void), const char* FuncName, const int FuncLineNum);

#ifdef HOST_HEAP_ARENA
static int leaks;
#endif

#ifdef DEVICE_BUILD
static void (*watched_test)(void);
static int tests_run;
static int tests_judged;

/* Each present a test makes is one of its frames; see frame_watch.h. */
static void
run_watched(void) {
    frame_watch_test_begin();
    watched_test();
    const frame_watch_verdict_t verdict = frame_watch_test_end();
    tests_judged += verdict.frames > 0;
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, verdict.repeating,
                                  "work repeated frame after frame - see the FRAME_WATCH lines above");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, verdict.dropped, "the frame watch ran out of room, so it may have missed one");
}
#endif

void
suite_run_test_timed(void (*func)(void), const char* name, int line) {
#ifdef HOST_HEAP_ARENA
    /* Outside the timed window on both ends, same as the timer itself -
     * this must never be what widens it. */
    size_t blocks_before, bytes_before;
    heap_arena_snapshot(&blocks_before, &bytes_before);
    heap_arena_reset_peak();
#endif

#ifdef DEVICE_BUILD
    const int64_t started = esp_timer_get_time();
#else
    const clock_t started = clock();
#endif

#ifdef DEVICE_BUILD
    watched_test = func;
    tests_run++;
    UnityDefaultTestRun(run_watched, name, line);
    (void)frame_watch_test_end();
#else
    UnityDefaultTestRun(func, name, line);
#endif

#ifdef DEVICE_BUILD
    const int64_t elapsed_ms = (esp_timer_get_time() - started) / 1000;
#else
    const long elapsed_ms = (clock() - started) * 1000L / CLOCKS_PER_SEC;
#endif

#ifdef HOST_HEAP_ARENA
    /* A rise in outstanding blocks means the test freed fewer than it
     * allocated. A fixture that asserts before freeing skips
     * every earlier free() and starves every test that runs after it. Own greppable line, no
     * consumer parses it today, so its shape is free to be whatever reads
     * clearest. */
    size_t blocks_after, bytes_after;
    heap_arena_snapshot(&blocks_after, &bytes_after);
    if (blocks_after > blocks_before) {
        printf("LEAK test=%s blocks=%zu bytes=%zu\n", name, blocks_after - blocks_before, bytes_after - bytes_before);
        leaks++;
    }
#endif

    /* Own sentinel line, same key=value shape as SELFTEST_COMPLETE - a new
     * line rather than an appended suffix, so the existing result line's
     * format never changes. int64_t because these range from under a
     * millisecond to the better part of eight minutes.
     *
     * name= and elapsed_ms= are parsed by the capture-validation and
     * performance-report scripts and must not move; a new field belongs at
     * the end, never between them. */
    printf("TEST_TIME name=%s elapsed_ms=%lld"
#ifdef HOST_HEAP_ARENA
           " peak_bytes=%zu"
#endif
           "\n",
           name, (long long)elapsed_ms
#ifdef HOST_HEAP_ARENA
           ,
           heap_arena_peak_bytes()
#endif
    );
}

int
suite_leaks(void) {
#ifdef HOST_HEAP_ARENA
    return leaks;
#else
    return 0;
#endif
}

void
suite_report_frame_watch(void) {
#ifdef DEVICE_BUILD
    printf("FRAME_WATCH judged %d of %d tests\n", tests_judged, tests_run);
    tests_run = 0;
    tests_judged = 0;
#endif
}
