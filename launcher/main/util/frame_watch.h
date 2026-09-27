/*
 * frame_watch - work that repeats frame after frame: a heap allocation, a
 * free or a console write whose call site turns up in most recent frames.
 * One such event on the frame something happened is not a finding; the same
 * site in FRAME_WATCH_REPEATS of the last FRAME_WATCH_WINDOW frames is.
 *
 * Pure here, on a frame_watch_t a test can own, with time passed in. On the
 * chip a development build feeds the shared instance from the heap's own
 * hooks and the log's vprintf; the host render harness feeds its own from
 * wrapped malloc/free and a captured stdout. Release compiles none of it.
 *
 * A site is whatever tells two call sites apart: the caller's address for
 * the heap, the format string for a log line.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* The board runs 30-60 frames a second, so the window is a quarter to half
 * a second: a site in half of it is steady state, while a periodic report
 * every second or two never reaches that. */
#define FRAME_WATCH_WINDOW             16
#define FRAME_WATCH_REPEATS            8

/* Frames after a restart - an app entered, a test begun - that are counted
 * but not judged, so a cache filled over the first few frames is not a
 * finding. */
#define FRAME_WATCH_WARMUP             16

#define FRAME_WATCH_SITES              16
#define FRAME_WATCH_REPORT_INTERVAL_US (10 * 1000 * 1000)

_Static_assert(FRAME_WATCH_WINDOW <= 16, "a site's history is a uint16_t");
_Static_assert(FRAME_WATCH_REPEATS > 1 && FRAME_WATCH_REPEATS <= FRAME_WATCH_WINDOW,
               "one frame alone must never make a site repeating");

typedef enum {
    FRAME_WATCH_ALLOC,
    FRAME_WATCH_FREE,
    FRAME_WATCH_CONSOLE,
    FRAME_WATCH_KINDS,
} frame_watch_kind_t;

typedef struct {
    uintptr_t site;
    uint16_t seen; /* bit 0 the frame still open, bit n the frame n before it */
    uint8_t kind;
    bool repeating;
    bool reported;
    int64_t reported_us;
} frame_watch_site_t;

typedef struct {
    frame_watch_site_t sites[FRAME_WATCH_SITES];
    int warmup_left;
    uint32_t frames;
    uint32_t open[FRAME_WATCH_KINDS];
    uint32_t last[FRAME_WATCH_KINDS];
    uint32_t dropped;
    int repeating;
    int ever_repeating;
} frame_watch_t;

static inline void
frame_watch_reset(frame_watch_t* w) {
    *w = (frame_watch_t){.warmup_left = FRAME_WATCH_WARMUP};
}

static inline const char*
frame_watch_kind_name(frame_watch_kind_t kind) {
    switch (kind) {
        case FRAME_WATCH_ALLOC: return "alloc";
        case FRAME_WATCH_FREE: return "free";
        case FRAME_WATCH_CONSOLE: return "console";
        case FRAME_WATCH_KINDS: break;
    }
    return "?";
}

static inline int
frame_watch_frames_seen(const frame_watch_site_t* s) {
    return __builtin_popcount(s->seen);
}

/* A slot is free once its site has gone a whole window unseen. A full table
 * drops the event from site tracking but still counts it. */
static inline void
frame_watch_note(frame_watch_t* w, frame_watch_kind_t kind, uintptr_t site) {
    w->open[kind]++;
    if (w->warmup_left > 0) {
        return;
    }
    frame_watch_site_t* free_slot = NULL;
    for (int i = 0; i < FRAME_WATCH_SITES; i++) {
        frame_watch_site_t* s = &w->sites[i];
        if (s->seen != 0 && s->site == site && s->kind == kind) {
            s->seen |= 1u;
            return;
        }
        if (s->seen == 0 && free_slot == NULL) {
            free_slot = s;
        }
    }
    if (free_slot == NULL) {
        w->dropped++;
        return;
    }
    *free_slot = (frame_watch_site_t){.site = site, .seen = 1u, .kind = (uint8_t)kind};
}

/* Judges the frame still open and starts the next one. Returns how many
 * sites became repeating with it. */
static inline int
frame_watch_close_frame(frame_watch_t* w) {
    for (int k = 0; k < FRAME_WATCH_KINDS; k++) {
        w->last[k] = w->open[k];
        w->open[k] = 0;
    }
    if (w->warmup_left > 0) {
        w->warmup_left--;
        return 0;
    }
    w->frames++;
    int became = 0;
    w->repeating = 0;
    const unsigned window_mask = (1u << FRAME_WATCH_WINDOW) - 1u;
    for (int i = 0; i < FRAME_WATCH_SITES; i++) {
        frame_watch_site_t* s = &w->sites[i];
        const bool repeating = frame_watch_frames_seen(s) >= FRAME_WATCH_REPEATS;
        if (repeating && !s->repeating) {
            became++;
        }
        s->repeating = repeating;
        w->repeating += repeating;
        s->seen = (uint16_t)(((unsigned)s->seen << 1) & window_mask);
    }
    w->ever_repeating += became;
    return became;
}

/* True when a repeating site should be warned about now: the first time,
 * then at most once per FRAME_WATCH_REPORT_INTERVAL_US while it keeps
 * repeating. Records the warning as given. */
static inline bool
frame_watch_take_due(frame_watch_site_t* s, int64_t now_us) {
    if (!s->repeating) {
        return false;
    }
    if (s->reported && now_us - s->reported_us < FRAME_WATCH_REPORT_INTERVAL_US) {
        return false;
    }
    s->reported = true;
    s->reported_us = now_us;
    return true;
}

/* Room for the counts and three repeating sites; a site that does not fit
 * is left out rather than cut in half. */
#define FRAME_WATCH_JSON_MAX 256

/* The last closed frame's counts and every repeating site that fits, as
 * one JSON object. Always terminated; returns its length. */
static inline int
frame_watch_format_json(const frame_watch_t* w, char* out, size_t out_size) {
    if (out_size == 0) {
        return 0;
    }
    int length = snprintf(out, out_size,
                          "{\"frames\":%lu,\"allocs\":%lu,\"frees\":%lu,\"console\":%lu,\"repeating\":%d,"
                          "\"dropped\":%lu,\"sites\":[",
                          (unsigned long)w->frames, (unsigned long)w->last[FRAME_WATCH_ALLOC],
                          (unsigned long)w->last[FRAME_WATCH_FREE], (unsigned long)w->last[FRAME_WATCH_CONSOLE],
                          w->repeating, (unsigned long)w->dropped);
    const size_t closing = 2; /* "]}" */
    if (length < 0 || (size_t)length + closing >= out_size) {
        out[0] = '\0';
        return 0;
    }
    bool first = true;
    for (int i = 0; i < FRAME_WATCH_SITES; i++) {
        const frame_watch_site_t* s = &w->sites[i];
        if (!s->repeating) {
            continue;
        }
        char entry[80];
        const int wrote = snprintf(entry, sizeof entry, "%s{\"kind\":\"%s\",\"site\":\"0x%08lx\"}", first ? "" : ",",
                                   frame_watch_kind_name((frame_watch_kind_t)s->kind), (unsigned long)s->site);
        if (wrote < 0 || (size_t)length + (size_t)wrote + closing >= out_size) {
            break;
        }
        memcpy(out + length, entry, (size_t)wrote);
        length += wrote;
        first = false;
    }
    memcpy(out + length, "]}", closing + 1);
    return length + (int)closing;
}

#if defined(ESP_PLATFORM) && CONFIG_LAUNCHER_DEVELOPMENT
#define FRAME_WATCH_ENABLED 1
#else
#define FRAME_WATCH_ENABLED 0
#endif

#if FRAME_WATCH_ENABLED
/* The calling task becomes the watched frame task; installs the log hook.
 * Safe to call again. */
void frame_watch_start(void);

/* A task whose work belongs to the frame while one is open: the panel's
 * sender, a second core's worker. */
void frame_watch_add_task(void* task);

void frame_watch_frame_begin(void);

/* Closes the frame and warns, once per site and interval, about every site
 * repeating now. */
void frame_watch_frame_end(void);

/* The next frames start a fresh warm-up: what is drawn has changed. */
void frame_watch_settle(void);

int frame_watch_json(char* out, size_t out_size);

/* A self-test's own watch, over its own instance, in which each present
 * closes a frame; the shell's is set aside until test_end, which returns
 * how many sites became repeating. */
void frame_watch_test_begin(void);
int frame_watch_test_end(void);
void frame_watch_presented(void);
#else
static inline void
frame_watch_start(void) {}

static inline void
frame_watch_add_task(void* task) {
    (void)task;
}

static inline void
frame_watch_frame_begin(void) {}

static inline void
frame_watch_frame_end(void) {}

static inline void
frame_watch_settle(void) {}

static inline void
frame_watch_test_begin(void) {}

static inline int
frame_watch_test_end(void) {
    return 0;
}

static inline void
frame_watch_presented(void) {}
#endif
