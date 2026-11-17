/* suite: self-registering test entry points for a caller-owned runner. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

/* The longest suite name a RUNSUITE request can carry; a longer one is refused at compile time. */
#define SUITE_NAME_MAX 38

typedef void (*suite_fn)(void);

typedef struct suite_entry {
    const char* name;
    suite_fn fn;
    bool on_request;
    struct suite_entry* next; /* set by suite_link(): the registry is a list the entries thread themselves into */
} suite_entry_t;

int suites_run(bool on_request);
size_t suites_count(void);
/* Entries are in name order. */
const suite_entry_t* suites_at(size_t i);
/* Runners must fail on dropped suites: a boot log alone can hide tests that never ran. */
int suites_dropped(void);
/* Engine metadata shares the registration failure count. */
void suites_record_drop(void);

/* Each SUITE_REGISTER owns its entry, so the registry is as large as the set registered: there is no cap to raise. */
void suite_link(suite_entry_t* entry);

#define SUITE_ENTRY(fn, on_request_)                                                                                   \
    _Static_assert(sizeof #fn <= SUITE_NAME_MAX + 1, #fn " is over SUITE_NAME_MAX");                                   \
    static suite_entry_t fn##_entry = {#fn, fn, on_request_, NULL};                                                    \
    __attribute__((constructor)) static void fn##_register(void) { suite_link(&fn##_entry); }

/* SUITE_REGISTER_ON_REQUEST suites run only when the caller explicitly selects them. */
#define SUITE_REGISTER(fn)            SUITE_ENTRY(fn, false)
#define SUITE_REGISTER_ON_REQUEST(fn) SUITE_ENTRY(fn, true)
