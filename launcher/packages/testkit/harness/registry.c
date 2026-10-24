/* registry: shared suite registration and name-ordered access. */
#include "testkit/suite.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static suite_entry_t suites[TESTKIT_SUITE_MAX];
static size_t suite_count;
static int dropped;
static bool sorted;

static void
register_suite(const char* name, suite_fn run, bool on_request) {
    if (suite_count == TESTKIT_SUITE_MAX) {
        printf("SUITE OVERFLOW: '%s' was not registered (max %d)\n", name, TESTKIT_SUITE_MAX);
        suites_record_drop();
        return;
    }
    sorted = false;
    suites[suite_count++] = (suite_entry_t){name, run, on_request};
}

void
suite_register(const char* name, suite_fn run) {
    register_suite(name, run, false);
}

void
suite_register_on_request(const char* name, suite_fn run) {
    register_suite(name, run, true);
}

static int
by_name(const void* a, const void* b) {
    return strcmp(((const suite_entry_t*)a)->name, ((const suite_entry_t*)b)->name);
}

size_t
suites_count(void) {
    return suite_count;
}

const suite_entry_t*
suites_at(size_t i) {
    if (!sorted) {
        qsort(suites, suite_count, sizeof suites[0], by_name);
        sorted = true;
    }
    return i < suite_count ? &suites[i] : NULL;
}

void
suites_record_drop(void) {
    dropped++;
}

int
suites_dropped(void) {
    return dropped;
}
