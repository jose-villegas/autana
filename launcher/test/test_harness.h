/* The device RUN_TEST wrapper's verdict, for tearDown() and the harness's
 * own test; timing.h, force-included into every suite, stays the override. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "util/runtime/frame_watch.h"

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
 * verdict, judging nothing, however body ends. For the harness's own test:
 * it leaves Unity's abort frame spent and the frame watch ended, and that
 * caller restores both. */
void suite_run_body(void (*body)(void), suite_test_verdict_t* verdict);

/* Asserts RUN_TEST's verdict; from tearDown(), under Unity's own frame. */
void suite_judge_watched_test(void);
