/*
 * console_latch - one verb's request for the frame loop to consume, set by
 * the console's reader task and taken by the shell once per set.
 *
 * `pending` alone is enough of a handoff: the reader task is the only
 * writer of `args`, and always writes it before setting `pending` true; the
 * frame loop only ever reads `args` after observing `pending` true and is
 * the only writer of `pending` back to false. A `bool` load/store is
 * atomic on this chip.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "console/console_verbs.h"

#define CONSOLE_ARGS_MAX CONSOLE_LINE_MAX

typedef struct {
    volatile bool pending;
    char args[CONSOLE_ARGS_MAX];
} console_latch_t;

/* As much of `from` as `into` holds, always terminated. */
static inline void
console_latch_copy(char* into, size_t into_size, const char* from) {
    if (into_size == 0) {
        return;
    }
    size_t length = strlen(from);
    if (length > into_size - 1) {
        length = into_size - 1;
    }
    memcpy(into, from, length);
    into[length] = '\0';
}

/* The reader task: records a new request, truncating args that do not fit
 * rather than overflowing. A newer call before the frame loop has taken the
 * previous one overwrites it - the latest line is what the device actually
 * saw most recently, so that is what a late frame loop should act on. */
static inline void
console_latch_set(console_latch_t* latch, const char* args) {
    console_latch_copy(latch->args, sizeof(latch->args), args);
    latch->pending = true;
}

/* The frame loop: true and `out` filled at most once per console_latch_set()
 * call since the previous take. False, and `out` left untouched, otherwise. */
static inline bool
console_latch_take(console_latch_t* latch, char* out, size_t out_size) {
    if (!latch->pending) {
        return false;
    }
    latch->pending = false;
    console_latch_copy(out, out_size, latch->args);
    return true;
}
