/* registry: name-ordered suites for standalone package tests. */
#include "registry.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "testkit/suite.h"

#define TESTKIT_SUITE_MAX 256

typedef struct {
    const char* name;
    suite_fn run;
    bool on_request;
} testkit_suite_t;

static testkit_suite_t suites[TESTKIT_SUITE_MAX];
static size_t suite_count;
static int dropped;

static void
register_suite(const char* name, suite_fn run, bool on_request) {
    if (suite_count == TESTKIT_SUITE_MAX) {
        fprintf(stderr, "Suite registry full: %s\n", name);
        dropped++;
        return;
    }
    suites[suite_count++] = (testkit_suite_t){name, run, on_request};
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
    return strcmp(((const testkit_suite_t*)a)->name, ((const testkit_suite_t*)b)->name);
}

int
testkit_run(int argc, char** argv) {
    qsort(suites, suite_count, sizeof suites[0], by_name);
    int errors = dropped;
    if (argc == 2 && strcmp(argv[1], TESTKIT_LIST_OPTION) == 0) {
        for (size_t i = 0; i < suite_count; i++) {
            puts(suites[i].name);
        }
        return errors;
    }
    if (argc == 1) {
        for (size_t i = 0; i < suite_count; i++) {
            if (!suites[i].on_request) {
                suites[i].run();
            }
        }
        return errors;
    }
    for (int arg = 1; arg < argc; arg++) {
        bool found = false;
        for (size_t i = 0; i < suite_count; i++) {
            if (strcmp(argv[arg], suites[i].name) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            fprintf(stderr, "Unknown suite: %s\n", argv[arg]);
            errors++;
        }
    }
    for (size_t i = 0; i < suite_count; i++) {
        for (int arg = 1; arg < argc; arg++) {
            if (strcmp(argv[arg], suites[i].name) == 0) {
                suites[i].run();
                break;
            }
        }
    }
    return errors;
}
