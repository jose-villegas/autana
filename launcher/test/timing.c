/*
 * Implements the RUN_TEST override declared in timing.h.
 *
 * Wraps Unity's own dispatcher (UnityDefaultTestRun) from outside: the timer
 * starts before it and stops after it returns, so nothing here runs inside
 * setUp(), the test body, or tearDown(). That matters because a handful of
 * tests time their own subject with timing_now_us() around a narrower
 * window (one call of the thing under test, say); this must never be what
 * widens that window.
 *
 * The elapsed-time line is printed AFTER UnityDefaultTestRun returns, so it
 * never touches the existing "file:line:name:PASS" line, which
 * launcher/tools/sweeps/validate_capture.py and an app's own
 * performance-report script already parse.
 */
#include "timing.h"

#include "suites.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef DEVICE_BUILD
#include "core/timing.h"
#else
#include <time.h>
#endif

#ifdef HOST_HEAP_ARENA
#include "heap_arena.h"
#endif

#ifdef DEVICE_BUILD
#include <stdatomic.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gfx/present/gfx_debug.h"
#include "test_harness.h"
#include "unity.h"

#include "esp_freertos_hooks.h" /* after FreeRTOS.h, which it needs */
#endif

/* Not pulled from unity.h: that header only declares this when RUN_TEST is
 * NOT already defined (see timing.h's top comment), the opposite of this
 * file's own situation, since it is what RUN_TEST now expands to. The real
 * definition lives in Unity's own unity.c/UnityDefaultTestRun and is
 * untouched; this is only the prototype, hand-matched to it. */
extern void UnityDefaultTestRun(void (*Func)(void), const char* FuncName, const int FuncLineNum);

#ifdef DEVICE_BUILD
/* Suites run on the main task. An overflow trips FreeRTOS's canary only at a
 * context switch and resets the chip, so a test that leaves less than an
 * interrupt's saved context plus a log line fails by name instead. */
#define STACK_RESERVE_BYTES 512U

/* A task deleted while it still runs, on either core, is freed later by an
 * idle task, not by vTaskDelete(). Before calling a drop a leak, the check
 * waits this long at most for the task count, and again for each core's idle
 * task, so a task left running is still caught. */
#define REAP_WAIT_MS        100U

static void (*watched_test)(void);
static suite_test_verdict_t watched_verdict;
static int tests_run;
static int tests_judged;
/* gfx_mode_denials() already in some test's verdict. */
static unsigned denials_judged;

/* Unity has one abort frame: a TEST_PASS() or a failed assertion jumps
 * straight to it. A frame here ends fn alone, so what follows it still runs.
 * Nothing is saved, to spare the main task's stack, so nothing after it may
 * assert until tearDown(): the frame it leaves is dead. */
static __attribute__((noinline)) void
call_protected(void (*fn)(void)) {
    if (TEST_PROTECT()) {
        fn();
    }
}

static atomic_uint idle_passes[portNUM_PROCESSORS];

/* Runs once per pass of a core's idle task, after that pass has freed the
 * tasks it took off the list of deleted ones. */
static bool
count_idle_pass(void) {
    atomic_fetch_add(&idle_passes[xPortGetCoreID()], 1);
    return true;
}

static void
count_idle_passes(void) {
    static bool counting;
    if (!counting) {
        for (int core = 0; core < portNUM_PROCESSORS; core++) {
            ESP_ERROR_CHECK(esp_register_freertos_idle_hook_for_cpu(count_idle_pass, core));
        }
        counting = true;
    }
}

/* Until every task the body ended is freed. The task count stays up until an
 * idle task takes the task off its list, and that idle task frees it just
 * after, so once the count is down each core's idle task finishes a pass. */
static void
await_ended_tasks(UBaseType_t tasks_before) {
    const TickType_t wait_max = pdMS_TO_TICKS(REAP_WAIT_MS);
    TickType_t waited = 0;
    for (; uxTaskGetNumberOfTasks() > tasks_before && waited < wait_max; waited++) {
        vTaskDelay(1);
    }
    for (int core = 0; core < portNUM_PROCESSORS; core++) {
        const unsigned passes = atomic_load(&idle_passes[core]);
        for (waited = 0; atomic_load(&idle_passes[core]) == passes && waited < wait_max; waited++) {
            vTaskDelay(1);
        }
    }
}

static size_t
drop(size_t before, size_t after) {
    return before > after ? before - after : 0;
}

static bool
heap_below(size_t free_8bit, size_t free_32bit) {
    return drop(free_8bit, heap_caps_get_free_size(MALLOC_CAP_8BIT)) > 0
           || drop(free_32bit, heap_caps_get_free_size(MALLOC_CAP_32BIT)) > 0;
}

/* Whether the heap is still below where it was once any task the body ended
 * is freed; only a drop is worth that wait. */
static bool
heap_dropped(size_t free_8bit, size_t free_32bit, UBaseType_t tasks_before) {
    if (!heap_below(free_8bit, free_32bit)) {
        return false;
    }
    await_ended_tasks(tasks_before);
    return heap_below(free_8bit, free_32bit);
}

/* Inlined into both callers: under RUN_TEST, a frame of its own is 32 bytes
 * less stack for every test body. */
static inline __attribute__((always_inline)) void
run_body(void (*body)(void), suite_test_verdict_t* verdict) {
    size_t free_8bit = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    size_t free_32bit = heap_caps_get_free_size(MALLOC_CAP_32BIT);
    UBaseType_t tasks_before = uxTaskGetNumberOfTasks();
    count_idle_passes();
    const size_t stack_free_before = uxTaskGetStackHighWaterMark(NULL);
    const unsigned denials_before = gfx_mode_denials();
    const unsigned judged_before = denials_judged;

    frame_watch_test_begin();
    call_protected(body);
    call_protected(suite_run_test_cleanup);
    verdict->watch = frame_watch_test_end();
    verdict->stack_free = uxTaskGetStackHighWaterMark(NULL);
    verdict->stack_deepened = verdict->stack_free < stack_free_before;

    /* A first run may fill a cache that lives on; only memory a second run
     * loses again is a leak. */
    const bool judged = !Unity.CurrentTestFailed && !Unity.CurrentTestIgnored;
    if (judged && heap_dropped(free_8bit, free_32bit, tasks_before)) {
        free_8bit = heap_caps_get_free_size(MALLOC_CAP_8BIT);
        free_32bit = heap_caps_get_free_size(MALLOC_CAP_32BIT);
        tasks_before = uxTaskGetNumberOfTasks();
        call_protected(body);
        call_protected(suite_run_test_cleanup);
        (void)heap_dropped(free_8bit, free_32bit, tasks_before);
        /* The second run is for the heap alone; its frames are not judged. */
        (void)frame_watch_test_end();
    }
    /* Both runs count; a run nested in this one judged its own already. */
    verdict->layouts_denied = gfx_mode_denials() - denials_before - (denials_judged - judged_before);
    denials_judged += verdict->layouts_denied;
    verdict->leaked_8bit = drop(free_8bit, heap_caps_get_free_size(MALLOC_CAP_8BIT));
    verdict->leaked_32bit = drop(free_32bit, heap_caps_get_free_size(MALLOC_CAP_32BIT));
}

void
suite_run_body(void (*body)(void), suite_test_verdict_t* verdict) {
    run_body(body, verdict);
}

/* Each present a test makes is one of its frames; see frame_watch.h. */
static void
run_watched(void) {
    watched_verdict = (suite_test_verdict_t){0};
    run_body(watched_test, &watched_verdict);
    tests_judged += watched_verdict.watch.frames > 0;
}

void
suite_judge_watched_test(void) {
    const suite_test_verdict_t* verdict = &watched_verdict;
    if (verdict->stack_deepened) {
        TEST_ASSERT_GREATER_OR_EQUAL_UINT32_MESSAGE(STACK_RESERVE_BYTES, verdict->stack_free,
                                                    "the test left the main task's stack almost full");
    }
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, verdict->leaked_8bit, "the test leaked 8-bit heap on its second run");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, verdict->leaked_32bit, "the test leaked 32-bit heap on its second run");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, verdict->watch.repeating,
                                  "work repeated frame after frame - see the FRAME_WATCH lines above");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, verdict->watch.dropped,
                                     "the frame watch ran out of room, so it may have missed one");
    /* A refused layout leaves the test running a fallback it never meant to
     * test, so it fails here rather than passing on that fallback. */
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, verdict->layouts_denied, "gfx refused a layout the test asked for");
}
#endif

void
suite_run_test_timed(void (*func)(void), const char* name, int line) {
    if (!suites_test_runs(name)) {
        return;
    }
    suite_clear_test_cleanup();
#ifdef HOST_HEAP_ARENA
    heap_arena_reset_peak();
#endif
#ifdef DEVICE_BUILD
    const int64_t started = timing_now_us();
#else
    const clock_t started = clock();
#endif

#ifdef DEVICE_BUILD
    watched_test = func;
    tests_run++;
    UnityDefaultTestRun(run_watched, name, line);
    const unsigned long stack_free_bytes = (unsigned long)uxTaskGetStackHighWaterMark(NULL);
#else
    UnityDefaultTestRun(func, name, line);
#endif

#ifdef DEVICE_BUILD
    const int64_t elapsed_ms = (timing_now_us() - started) / 1000;
#else
    const long elapsed_ms = (clock() - started) * 1000L / CLOCKS_PER_SEC;
#endif

    /* Own sentinel line, same key=value shape as SELFTEST_COMPLETE; a new
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
#ifdef DEVICE_BUILD
           " stack_free_bytes=%lu"
#endif
           "\n",
           name, (long long)elapsed_ms
#ifdef HOST_HEAP_ARENA
           ,
           heap_arena_peak_bytes()
#endif
#ifdef DEVICE_BUILD
               ,
           stack_free_bytes
#endif
    );
}

/* Static, not on the stack: the test runs inside this call, on the main
 * task's stack, and Unity keeps a pointer to the name until it returns. */
static char case_name[SUITE_TEST_NAME_MAX + 1];

void
suite_run_test_case(void (*func)(void), const char* name, const char* const* segments, size_t count, int line) {
    size_t length = strlen(name);
    bool named = length <= SUITE_TEST_NAME_MAX;
    if (named) {
        memcpy(case_name, name, length);
    }
    for (size_t i = 0; named && i < count; i++) {
        const size_t segment = segments[i] != NULL ? strlen(segments[i]) : 0;
        if (segment == 0) {
            continue;
        }
        named = length + 1 + segment <= SUITE_TEST_NAME_MAX && strspn(segments[i], SUITE_TEST_SEGMENT_CHARS) == segment;
        if (named) {
            case_name[length] = SUITE_TEST_SEPARATOR;
            memcpy(case_name + length + 1, segments[i], segment);
            length += 1 + segment;
        }
    }
    if (!named) {
        suites_record_drop();
        printf("SUITE OVERFLOW: a case of %s is past SUITE_TEST_NAME_MAX %d or not of SUITE_TEST_SEGMENT_CHARS\n", name,
               SUITE_TEST_NAME_MAX);
        return;
    }
    case_name[length] = '\0';
    suite_run_test_timed(func, case_name, line);
}

void
suite_report_frame_watch(void) {
#ifdef DEVICE_BUILD
    printf("FRAME_WATCH judged %d of %d tests\n", tests_judged, tests_run);
    tests_run = 0;
    tests_judged = 0;
#endif
}
