/*
 * The suite registry, shared by both runners.
 *
 * Deliberately free of Unity and of anything platform-specific: it is a sorted
 * list of function pointers filled in before main(), so it links identically
 * into the host runner and the firmware.
 */

#include "suites.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    const char* name;
    suite_fn fn;
    bool on_request;
} suite_entry_t;

static suite_entry_t suites[SUITE_MAX];

static int registered;
static int dropped;

static void
register_suite(const char* name, suite_fn fn, bool on_request) {
    if (registered >= SUITE_MAX) {
        /* Counted as well as printed. A dropped suite is a test that did not
         * run, and a printf on its own leaves that as one line in a boot log
         * nobody reads, directly above a summary saying everything passed.
         * suites_dropped() is what turns the run red - see both runners. */
        dropped++;
        printf("SUITE OVERFLOW: '%s' was not registered (max %d)\n", name, SUITE_MAX);
        return;
    }
    suites[registered].name = name;
    suites[registered].fn = fn;
    suites[registered].on_request = on_request;
    registered++;
}

void
suite_register(const char* name, suite_fn fn) {
    register_suite(name, fn, false);
}

void
suite_register_on_request(const char* name, suite_fn fn) {
    register_suite(name, fn, true);
}

int
suites_dropped(void) {
    return dropped;
}

void
suites_run_all(void) {
    /* Link order decides constructor order, so sort to keep the output stable
     * between builds. */
    for (int i = 1; i < registered; i++) {
        const suite_entry_t key = suites[i];
        int j = i - 1;
        while (j >= 0 && strcmp(suites[j].name, key.name) > 0) {
            suites[j + 1] = suites[j];
            j--;
        }
        suites[j + 1] = key;
    }

    for (int i = 0; i < registered; i++) {
        if (!suites[i].on_request) {
            suites[i].fn();
        }
    }
}

bool
suites_run_one(const char* name) {
    for (int i = 0; i < registered; i++) {
        if (strcmp(suites[i].name, name) == 0) {
            suites[i].fn();
            return true;
        }
    }
    return false;
}
