/* suite: self-registering test entry points for a caller-owned runner. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#define TESTKIT_SUITE_MAX 256
/* The longest suite name a RUNSUITE request can carry; a longer one is refused at compile time. */
#define SUITE_NAME_MAX    38

typedef void (*suite_fn)(void);

typedef struct {
    const char* name;
    suite_fn fn;
    bool on_request;
} suite_entry_t;

int suites_run(bool on_request);
size_t suites_count(void);
/* Entries are sorted by name on first access. */
const suite_entry_t* suites_at(size_t i);
/* Runners must fail on dropped suites: a boot log alone can hide tests that never ran. */
int suites_dropped(void);
/* Engine metadata shares the registration failure count. */
void suites_record_drop(void);

/* On-request suites run only when the caller explicitly selects them. */
void suite_register(const char* name, suite_fn fn);
void suite_register_on_request(const char* name, suite_fn fn);

#define SUITE_REGISTER(fn)                                                                                             \
    _Static_assert(sizeof #fn <= SUITE_NAME_MAX + 1, #fn " is over SUITE_NAME_MAX");                                   \
    __attribute__((constructor)) static void fn##_register(void) { suite_register(#fn, fn); }

#define SUITE_REGISTER_ON_REQUEST(fn)                                                                                  \
    _Static_assert(sizeof #fn <= SUITE_NAME_MAX + 1, #fn " is over SUITE_NAME_MAX");                                   \
    __attribute__((constructor)) static void fn##_register(void) { suite_register_on_request(#fn, fn); }
