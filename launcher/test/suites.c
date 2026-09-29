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

typedef enum {
    FILTER_OFF,    /* every test runs */
    FILTER_SURVEY, /* no test runs; each is listed and the patterns tallied */
    FILTER_RUN,    /* only the selected tests run */
} filter_phase_t;

static char patterns[SUITE_FILTER_MAX][SUITE_FILTER_LEN];
static int pattern_count;
static int pattern_hits[SUITE_FILTER_MAX];
static int selected_count;
static filter_phase_t phase;
static void (*survey_hook)(bool quiet);

bool
suites_filter_add(const char* pattern) {
    const size_t length = strlen(pattern);
    if (length == 0 || length >= SUITE_FILTER_LEN || pattern_count >= SUITE_FILTER_MAX) {
        return false;
    }
    memcpy(patterns[pattern_count], pattern, length + 1);
    pattern_count++;
    return true;
}

void
suites_filter_clear(void) {
    pattern_count = 0;
}

/* Counts a hit for every pattern the name contains, so one that matches
 * nothing stays visible even when another selects the same test. */
static bool
name_matches_a_pattern(const char* test_name) {
    bool any = false;
    for (int i = 0; i < pattern_count; i++) {
        if (strstr(test_name, patterns[i]) != NULL) {
            pattern_hits[i]++;
            any = true;
        }
    }
    return any;
}

bool
suites_test_runs(const char* test_name) {
    if (phase == FILTER_OFF) {
        selected_count++;
        return true;
    }
    const bool matches = name_matches_a_pattern(test_name);
    if (phase == FILTER_SURVEY) {
        printf("SUITE_TEST name=%s selected=%d\n", test_name, matches ? 1 : 0);
        return false;
    }
    selected_count += matches ? 1 : 0;
    return matches;
}

int
suites_filter_unmatched(void) {
    int unmatched = 0;
    for (int i = 0; i < pattern_count; i++) {
        unmatched += pattern_hits[i] == 0;
    }
    return unmatched;
}

int
suites_filter_selected(void) {
    return selected_count;
}

void
suites_set_survey_hook(void (*hook)(bool quiet)) {
    survey_hook = hook;
}

static void
run_filtered(const suite_entry_t* entry) {
    memset(pattern_hits, 0, sizeof pattern_hits);
    phase = FILTER_SURVEY;
    if (survey_hook != NULL) {
        survey_hook(true);
    }
    entry->fn();
    if (survey_hook != NULL) {
        survey_hook(false);
    }
    if (suites_filter_unmatched() == 0) {
        phase = FILTER_RUN;
        entry->fn();
    } else {
        for (int i = 0; i < pattern_count; i++) {
            if (pattern_hits[i] == 0) {
                printf("SUITE_FILTER_UNMATCHED pattern=%s\n", patterns[i]);
            }
        }
    }
    phase = FILTER_OFF;
}

bool
suites_run_one(const char* name) {
    for (int i = 0; i < registered; i++) {
        if (strcmp(suites[i].name, name) != 0) {
            continue;
        }
        selected_count = 0;
        if (pattern_count == 0) {
            suites[i].fn();
        } else {
            run_filtered(&suites[i]);
        }
        return true;
    }
    return false;
}
