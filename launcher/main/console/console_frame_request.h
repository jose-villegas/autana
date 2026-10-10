/*
 * console_frame_request: what console verbs ask of the frame loop, handed
 * from the console's reader task to the shell through one mailbox.
 *
 * A verb parses its line into a request and posts it; the shell takes
 * everything posted once per pass, before the freeze gate, so a held frame
 * still answers FRAMEWATCH and SCREENSHOT. The reader task may not act
 * itself: see console.c's top comment.
 *
 * Each kind keeps only its latest request, the line the device saw most
 * recently, except the CONSOLE_FRAME_HELD kinds: they run across frames, one
 * at a time, so a second is refused while the first waits or runs.
 *
 * Every call holds the mailbox's lock for its whole copy, so a take never
 * sees a kind's bit without its fields, nor fields a post is still writing.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "console/console_verbs.h"
#include "freertos/FreeRTOS.h"

/* RUNSUITE exists where the suites do: a self-test image and the host runner. */
#if CONFIG_LAUNCHER_SELFTEST || !defined(ESP_PLATFORM)
#include "console/console_runsuite.h"
#define CONSOLE_FRAME_HAS_RUNSUITE 1
#endif

typedef enum {
    CONSOLE_FRAME_NAVIGATE = 1u << 0,
    CONSOLE_FRAME_FRAMEWATCH = 1u << 1,
    CONSOLE_FRAME_SCREENSHOT = 1u << 2,
    CONSOLE_FRAME_FREEZE = 1u << 3,
    CONSOLE_FRAME_RUNSUITE = 1u << 4,
} console_frame_kind_t;

/* Taken into the mailbox's busy set until console_frame_done(). */
#define CONSOLE_FRAME_HELD CONSOLE_FRAME_RUNSUITE

typedef enum {
    CONSOLE_NAVIGATION_APPS,
    CONSOLE_NAVIGATION_OPEN,
    CONSOLE_NAVIGATION_HOME,
} console_navigation_t;

/* A field is meaningful only while its kind's bit is in `kinds`. */
typedef struct {
    uint32_t kinds;
    console_navigation_t navigation;
    char app[CONSOLE_LINE_MAX]; /* OPEN's name */
    bool frozen;                /* FREEZE and STEP; RESUME clears it */
    int steps;                  /* STEP's frame count */
#ifdef CONSOLE_FRAME_HAS_RUNSUITE
    char suite[RUNSUITE_ARGS_MAX + 1]; /* a suite name, then any patterns */
#endif
} console_frame_request_t;

typedef struct {
    portMUX_TYPE lock;
    console_frame_request_t pending;
    uint32_t busy; /* held kinds taken and not yet done */
} console_frame_mailbox_t;

#define CONSOLE_FRAME_MAILBOX_INIT {.lock = portMUX_INITIALIZER_UNLOCKED}

/* The one the verbs post to and the shell takes from. */
console_frame_mailbox_t* console_frame_mailbox(void);

/* Any task: merges `request`'s kinds into the mailbox. False, and nothing
 * merged, when it holds a held kind already pending or busy. */
bool console_frame_post(console_frame_mailbox_t* mailbox, const console_frame_request_t* request);

/* The frame loop, once per pass: everything posted since the last take, or
 * kinds 0. Held kinds stay busy until console_frame_done(). */
void console_frame_take(console_frame_mailbox_t* mailbox, console_frame_request_t* out);

/* The frame loop: a held kind it took has finished. */
void console_frame_done(console_frame_mailbox_t* mailbox, console_frame_kind_t kind);
