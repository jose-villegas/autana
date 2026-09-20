/*
 * tune - numbers a developer can change on a running device, by name, over
 * the console: "SET ridge.trail 200". For a constant that is judged by eye,
 * where each guess otherwise costs a build and a flash.
 *
 * A tunable is an int32_t with a range, stated once, where it is used:
 *
 *     TUNE_OWNER(ridge);
 *     TUNE(ridge, trail, 226, 0, 255);
 *
 * On a development build that is a variable `trail` and an entry that puts
 * itself in the registry before app_main(), the way an app registers. On a
 * release build it is the constant 226 and nothing else: no registry, no
 * names, no variable. The source stays the truth; a value found on the device
 * is written back into its TUNE line, and nothing is kept across a reboot.
 *
 * A registry is a list threaded through the entries themselves, so there is
 * no table to fill. The protocol is pure - a line in, replies out through a
 * callback, against a registry a test can own - and the console listener is
 * the device's caller, against the shared one.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "build_variant.h"

#if !defined(ESP_PLATFORM) || CONFIG_LAUNCHER_DEVELOPMENT
#define TUNE_ENABLED 1
#else
#define TUNE_ENABLED 0
#endif

/* "SET <name> <value>" has to fit the console's line. */
#define TUNE_NAME_MAX 32

/* What bakes tunables into a table rebuilds when its owner's generation has
 * moved: it goes up on every SET or RESET of one of the owner's own. */
typedef struct {
    uint32_t generation;
} tune_owner_t;

typedef struct tune_entry {
    const char* name;
    int32_t* value;
    int32_t initial, low, high;
    tune_owner_t* owner;
    struct tune_entry* next;
} tune_entry_t;

typedef struct {
    tune_entry_t* first;
    tune_entry_t* clashed;
} tune_registry_t;

typedef void (*tune_reply_fn)(const char* line);

/* Kept in name order, so a listing does not depend on link order. False for a
 * name already taken by another entry: the first keeps it, and a listing
 * reports the clash. An entry registered twice is registered once. */
bool tune_register(tune_registry_t* registry, tune_entry_t* entry);

int tune_count(const tune_registry_t* registry);
const tune_entry_t* tune_find(const tune_registry_t* registry, const char* name);

/* True if `line` was one of these, whatever came of it. Replies, one call
 * each:
 *   SET <name> <value>   TUNE_OK <name>=<value>, or TUNE_ERR <reason> ...
 *   GET <name>           the same
 *   RESET <name>         the same, the value being the initial one again
 *   TUNE                 TUNE <name>=<value> min=<low> max=<high> default=<initial>
 *                        per tunable, TUNE_ERR clash <name> per name declared
 *                        twice, then TUNE_END count=<n> */
bool tune_registry_handle_line(tune_registry_t* registry, const char* line, tune_reply_fn reply);

#if TUNE_ENABLED

/* The one TUNE() entries join, and the console answers from. */
tune_registry_t* tune_shared(void);
bool tune_handle_line(const char* line, tune_reply_fn reply);

#define TUNE_OWNER(owner) static tune_owner_t owner##_tunables

#define TUNE(owner, what, initial_value, low_value, high_value)                                                        \
    _Static_assert((initial_value) >= (low_value) && (initial_value) <= (high_value),                                  \
                   #owner "." #what " starts outside its own range");                                                  \
    _Static_assert(sizeof(#owner "." #what) - 1 <= TUNE_NAME_MAX, #owner "." #what " is too long a name to SET");      \
    static int32_t what = (initial_value);                                                                             \
    static tune_entry_t what##_tunable;                                                                                \
    __attribute__((constructor)) static void what##_tunable_register(void) {                                           \
        tune_register(tune_shared(), &what##_tunable);                                                                 \
    }                                                                                                                  \
    static tune_entry_t what##_tunable = {#owner "." #what,  &what, (initial_value), (low_value), (high_value),        \
                                          &owner##_tunables, NULL}

#define TUNE_GENERATION(owner) (owner##_tunables.generation)

#else

#define TUNE_OWNER(owner) struct owner##_tunables_are_constants

#define TUNE(owner, what, initial_value, low_value, high_value)                                                        \
    _Static_assert((initial_value) >= (low_value) && (initial_value) <= (high_value),                                  \
                   #owner "." #what " starts outside its own range");                                                  \
    enum { what = (initial_value) }

#define TUNE_GENERATION(owner) 0u

#endif
