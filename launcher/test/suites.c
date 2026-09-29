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

static char patterns[SUITE_FILTER_MAX][SUITE_FILTER_LEN];
static int pattern_count;
static int pattern_hits[SUITE_FILTER_MAX];
static int selected_count;
static bool filtering;

/* Splits "a,b,c" into patterns. A refusal prints the offender, so the host
 * can tell a refused filter from one that matched nothing. */
static bool
parse_patterns(const char* list) {
    pattern_count = 0;
    while (*list != '\0') {
        const char* comma = strchr(list, ',');
        const size_t length = comma != NULL ? (size_t)(comma - list) : strlen(list);
        if (length == 0 || length >= SUITE_FILTER_LEN || pattern_count >= SUITE_FILTER_MAX) {
            printf("SUITE_FILTER_REFUSED pattern=%.*s\n", (int)(length < 60 ? length : 60), list);
            pattern_count = 0;
            return false;
        }
        memcpy(patterns[pattern_count], list, length);
        patterns[pattern_count][length] = '\0';
        pattern_count++;
        list += length + (comma != NULL ? 1 : 0);
    }
    return true;
}

bool
suites_test_runs(const char* test_name) {
    if (!filtering) {
        selected_count++;
        return true;
    }
    bool matches = false;
    for (int i = 0; i < pattern_count; i++) {
        if (strstr(test_name, patterns[i]) != NULL) {
            pattern_hits[i]++;
            matches = true;
        }
    }
    printf("SUITE_TEST name=%s selected=%d\n", test_name, matches ? 1 : 0);
    selected_count += matches ? 1 : 0;
    return matches;
}

static int
unmatched_patterns(void) {
    int unmatched = 0;
    for (int i = 0; i < pattern_count; i++) {
        unmatched += pattern_hits[i] == 0;
    }
    return unmatched;
}

suite_run_t
suites_run_request(const char* request) {
    suite_run_t run = {0};
    const char* space = strchr(request, ' ');
    const size_t name_length = space != NULL ? (size_t)(space - request) : strlen(request);
    const char* list = space != NULL ? space + 1 : "";
    if (name_length > SUITE_NAME_MAX) {
        return run;
    }
    memcpy(run.name, request, name_length);

    filtering = false;
    selected_count = 0;
    memset(pattern_hits, 0, sizeof pattern_hits);
    for (int i = 0; i < registered; i++) {
        if (strcmp(suites[i].name, run.name) != 0) {
            continue;
        }
        run.found = true;
        filtering = list[0] != '\0';
        if (filtering && !parse_patterns(list)) {
            filtering = false;
            run.refused = true;
            return run;
        }
        suites[i].fn();
        run.selected = selected_count;
        run.unmatched = filtering ? unmatched_patterns() : 0;
        break;
    }
    filtering = false;
    pattern_count = 0;
    return run;
}

void
suites_print_run(const suite_run_t* run) {
    printf("\nRUNSUITE_COMPLETE name=%s found=%d selected=%d unmatched=%d\n", run->name, run->found ? 1 : 0,
           run->selected, run->unmatched);
}
