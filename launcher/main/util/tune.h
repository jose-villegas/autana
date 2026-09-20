/*
 * tune - numbers a developer can change on a running device, by name, over
 * the console: "SET launcher.ridge_trail 200". For a constant that is judged
 * by eye, where each guess otherwise costs a build and a flash.
 *
 * A tunable is an int32_t with a range, declared where it is used with
 * TUNE_INT and made reachable with TUNE_REGISTER. On a release build the
 * first is a plain constant and the second nothing, so release carries no
 * registry, no names and no variable: the source stays the truth, and a value
 * found here is written back into it by hand. Nothing is kept across a
 * reboot.
 *
 * Portable: a line comes in and replies go out through a callback, so the
 * whole protocol runs on a host. util/screenshot.c's console listener is the
 * device's caller.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "build_variant.h"

#if !defined(ESP_PLATFORM) || CONFIG_LAUNCHER_DEVELOPMENT
#define TUNE_ENABLED 1
#else
#define TUNE_ENABLED 0
#endif

#define TUNE_MAX      64
#define TUNE_NAME_MAX 32

#if TUNE_ENABLED
#define TUNE_INT(variable, initial)              static int32_t variable = (initial)
#define TUNE_REGISTER(name, variable, low, high) tune_register((name), &(variable), (low), (high))
#else
#define TUNE_INT(variable, initial)              enum { variable = (initial) }
#define TUNE_REGISTER(name, variable, low, high) ((void)0)
#endif

typedef struct {
    const char* name;
    int32_t* value;
    int32_t low, high;
} tune_entry_t;

typedef void (*tune_reply_fn)(const char* line);

/* `name` must outlive the registry - a string literal. Registering a name
 * again moves it to the new variable, so a module may register on every
 * start. False when the table is full or the name too long for a SET line. */
bool tune_register(const char* name, int32_t* value, int32_t low, int32_t high);

int tune_count(void);
const tune_entry_t* tune_at(int index);

/* Goes up on every successful SET. A reader that bakes a value into a table
 * compares it against the one it last built for. */
uint32_t tune_generation(void);

/* True if `line` was one of "SET <name> <value>", "GET <name>" or "TUNE",
 * whatever came of it. Replies, one call each:
 *   TUNE_OK <name>=<value>
 *   TUNE_ERR <reason>
 *   TUNE <name>=<value> min=<low> max=<high>      per tunable, for "TUNE"
 *   TUNE_END count=<n>                            then this */
bool tune_handle_line(const char* line, tune_reply_fn reply);

/* Forgets every tunable. For a test. */
void tune_reset(void);
